/* bro-engine-executor.c */
#include "bro-engine-executor.h"

typedef struct {
  BroExecutorAction action;
  gpointer data;
  GDestroyNotify destroy;
} Posted;

struct _BroEngineExecutor {
  GObject parent_instance;
  GMutex lock;
  GQueue queue;           /* of Posted */
  gboolean drain_pending; /* an idle source will drain the queue */
  gboolean stopping;
  gboolean joined;
  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  GThreadPool *workers;
};

G_DEFINE_FINAL_TYPE (BroEngineExecutor, bro_engine_executor, G_TYPE_OBJECT)

static void
posted_free (Posted *p)
{
  if (p->destroy)
    p->destroy (p->data);
  g_free (p);
}

/* The executor's one source: runs everything queued, including what those actions post. */
static gboolean
drain (gpointer data)
{
  BroEngineExecutor *self = data;
  for (;;)
    {
      Posted *p;
      g_mutex_lock (&self->lock);
      p = g_queue_pop_head (&self->queue);
      if (!p)
        {
          self->drain_pending = FALSE;
          g_mutex_unlock (&self->lock);
          return G_SOURCE_REMOVE;
        }
      g_mutex_unlock (&self->lock);
      p->action (p->data);
      posted_free (p);
    }
}

/* The thread holds a reference until it ends, so an executor lives until it is stopped. */
static gpointer
run (gpointer data)
{
  BroEngineExecutor *self = data;
  g_main_context_push_thread_default (self->context);
  g_main_loop_run (self->loop);
  g_main_context_pop_thread_default (self->context);
  g_object_unref (self);
  return NULL;
}

/* Queues @p; the caller holds the lock. */
static void
enqueue_locked (BroEngineExecutor *self, Posted *p)
{
  g_queue_push_tail (&self->queue, p);
  if (!self->drain_pending)
    {
      GSource *source = g_idle_source_new ();
      self->drain_pending = TRUE;
      g_source_set_priority (source, G_PRIORITY_DEFAULT);
      g_source_set_callback (source, drain, self, NULL);
      g_source_attach (source, self->context);
      g_source_unref (source);
    }
}

gboolean
bro_engine_executor_post (BroEngineExecutor *self, BroExecutorAction action, gpointer data, GDestroyNotify destroy)
{
  Posted *p = g_new0 (Posted, 1);
  p->action = action;
  p->data = data;
  p->destroy = destroy;
  g_mutex_lock (&self->lock);
  if (self->stopping)
    {
      g_mutex_unlock (&self->lock);
      posted_free (p);
      return FALSE;
    }
  enqueue_locked (self, p);
  g_mutex_unlock (&self->lock);
  return TRUE;
}

gboolean
bro_engine_executor_is_current (BroEngineExecutor *self)
{
  return g_thread_self () == self->thread;
}

/* ---- workers ---- */

typedef struct {
  BroEngineExecutor *executor; /* a reference */
  BroWorkerFunc work;
  BroWorkerDone done;
  gpointer data;
  GDestroyNotify destroy;
  BroBroError *error;
} Job;

static void
job_free (gpointer data)
{
  Job *job = data;
  if (job->destroy)
    job->destroy (job->data);
  g_clear_pointer (&job->error, bro_bro_error_free);
  g_object_unref (job->executor);
  g_free (job);
}

static void
job_done (gpointer data)
{
  Job *job = data;
  job->done (job->data, job->error);
}

static void
job_run (gpointer data, gpointer unused)
{
  Job *job = data;
  if (!job->work (job->data, &job->error) && !job->error)
    job->error = bro_bro_error_new ("internal.unexpected", NULL, NULL);
  bro_engine_executor_post (job->executor, job_done, job, job_free);
}

void
bro_engine_executor_worker (BroEngineExecutor *self, BroWorkerFunc work, BroWorkerDone done, gpointer data,
                            GDestroyNotify destroy)
{
  Job *job = g_new0 (Job, 1);
  job->executor = g_object_ref (self);
  job->work = work;
  job->done = done;
  job->data = data;
  job->destroy = destroy;
  g_thread_pool_push (self->workers, job, NULL);
}

/* ---- barrier and stop ---- */

typedef struct {
  GMutex lock;
  GCond cond;
  gboolean reached;
} Barrier;

static void
barrier_reached (gpointer data)
{
  Barrier *b = data;
  g_mutex_lock (&b->lock);
  b->reached = TRUE;
  g_cond_signal (&b->cond);
  g_mutex_unlock (&b->lock);
}

void
bro_engine_executor_barrier (BroEngineExecutor *self)
{
  Barrier b = { 0 };
  g_return_if_fail (!bro_engine_executor_is_current (self));
  g_mutex_init (&b.lock);
  g_cond_init (&b.cond);
  if (bro_engine_executor_post (self, barrier_reached, &b, NULL))
    {
      g_mutex_lock (&b.lock);
      while (!b.reached)
        g_cond_wait (&b.cond, &b.lock);
      g_mutex_unlock (&b.lock);
    }
  g_mutex_clear (&b.lock);
  g_cond_clear (&b.cond);
}

static void
quit (gpointer data)
{
  BroEngineExecutor *self = data;
  g_main_loop_quit (self->loop);
}

void
bro_engine_executor_stop (BroEngineExecutor *self)
{
  Posted *p;
  g_mutex_lock (&self->lock);
  if (!self->stopping)
    {
      self->stopping = TRUE;
      p = g_new0 (Posted, 1);
      p->action = quit;
      p->data = self;
      enqueue_locked (self, p); /* after everything queued */
    }
  g_mutex_unlock (&self->lock);
  if (bro_engine_executor_is_current (self) || self->joined)
    return;
  g_thread_join (self->thread);
  self->joined = TRUE;
}

/* ---- the object ---- */

static void
bro_engine_executor_finalize (GObject *object)
{
  BroEngineExecutor *self = BRO_ENGINE_EXECUTOR (object);
  /* Only the thread's own last reference gets here (see run), as it ends, unless stop already joined it. */
  if (!self->joined)
    g_thread_unref (self->thread);
  g_thread_pool_free (self->workers, FALSE, FALSE);
  g_queue_clear_full (&self->queue, (GDestroyNotify) posted_free);
  g_main_loop_unref (self->loop);
  g_main_context_unref (self->context);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_engine_executor_parent_class)->finalize (object);
}

static void
bro_engine_executor_class_init (BroEngineExecutorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_engine_executor_finalize;
}

static void
bro_engine_executor_init (BroEngineExecutor *self)
{
  g_mutex_init (&self->lock);
  g_queue_init (&self->queue);
  self->context = g_main_context_new ();
  self->loop = g_main_loop_new (self->context, FALSE);
  self->workers = g_thread_pool_new (job_run, NULL, -1, FALSE, NULL);
}

BroEngineExecutor *
bro_engine_executor_new (const char *name)
{
  BroEngineExecutor *self = g_object_new (BRO_TYPE_ENGINE_EXECUTOR, NULL);
  self->thread = g_thread_new (name ? name : "bromelia-engine", run, g_object_ref (self));
  return self;
}
