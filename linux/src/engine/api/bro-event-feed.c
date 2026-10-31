/* bro-event-feed.c */
#include "bro-event-feed.h"
#include "bro-api-private.h"

struct _BroEventFeed {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventStream *stream;
  GHashTable *job_log; /* job ids */
  BroSnapshotFunc snapshot;
  GWeakRef owner;
  GMutex lock;
  BroEngineEvent *first; /* nullable; under the lock */
  GPtrArray *readers;    /* GThread *: the nexts waiting on the stream, under the lock */
};

G_DEFINE_FINAL_TYPE (BroEventFeed, bro_event_feed, G_TYPE_OBJECT)

typedef struct {
  BroEventFeed *feed; /* a reference */
  BroEventFeedDone done;
  gpointer data;
  GDestroyNotify destroy;
  BroEngineEvent *event; /* what done gets; nullable */
} Next;

static void
next_free (Next *n)
{
  if (n->destroy)
    n->destroy (n->data);
  g_clear_pointer (&n->event, bro_engine_event_unref);
  g_object_unref (n->feed);
  g_free (n);
}

static void
next_answer (gpointer data)
{
  Next *n = data;
  n->done (n->event, n->data);
  next_free (n);
}

/* Gives @event (taken; NULL: closed) to the reader on the executor, else (the executor stopped) here. */
static void
deliver (Next *n, BroEngineEvent *event)
{
  n->event = event;
  if (!bro_engine_executor_post (n->feed->executor, next_answer, n, NULL))
    next_answer (n);
}

static void
snapshot_ready (BroEngineEvent *event, gpointer data)
{
  Next *n = data;
  if (!event)
    bro_event_feed_close (n->feed); /* the engine stopped while the snapshot was asked for */
  n->event = event;
  next_answer (n);
}

static void
ask_snapshot (gpointer data)
{
  Next *n = data;
  g_autoptr (GObject) owner = g_weak_ref_get (&n->feed->owner);
  if (!owner)
    {
      snapshot_ready (NULL, n);
      return;
    }
  n->feed->snapshot (owner, snapshot_ready, n);
}

static gpointer
read_next (gpointer data)
{
  Next *n = data;
  BroEventFeed *self = n->feed;
  for (;;)
    {
      g_autoptr (BroStreamItem) item = bro_event_stream_next (self->stream);
      if (!item)
        {
          deliver (n, NULL);
          return NULL;
        }
      if (item->kind == BRO_STREAM_ITEM_RESYNC)
        {
          if (!bro_engine_executor_post (self->executor, ask_snapshot, n, NULL))
            {
              bro_event_feed_close (self);
              next_answer (n);
            }
          return NULL;
        }
      if (item->event->type == BRO_ENGINE_EVENT_TYPE_JOB_LOG
          && !g_hash_table_contains (self->job_log, bro_json_value_get_string (bro_json_value_member (item->event->data, "jobId"), "")))
        continue;
      deliver (n, bro_engine_event_ref (item->event));
      return NULL;
    }
}

void
bro_event_feed_next (BroEventFeed *self, BroEventFeedDone done, gpointer data, GDestroyNotify destroy)
{
  Next *n = g_new0 (Next, 1);
  BroEngineEvent *first;
  n->feed = g_object_ref (self);
  n->done = done;
  n->data = data;
  n->destroy = destroy;
  if (bro_event_stream_is_closed (self->stream))
    {
      deliver (n, NULL);
      return;
    }
  g_mutex_lock (&self->lock);
  first = g_steal_pointer (&self->first);
  if (!first)
    g_ptr_array_add (self->readers, g_thread_new ("bro-event-feed", read_next, n));
  g_mutex_unlock (&self->lock);
  if (first)
    deliver (n, first);
}

/* Joins the readers that aren't this thread (one that is goes on to its end by itself). */
static void
join_readers (BroEventFeed *self)
{
  g_autoptr (GPtrArray) readers = NULL;
  g_mutex_lock (&self->lock);
  readers = g_steal_pointer (&self->readers);
  self->readers = g_ptr_array_new ();
  g_mutex_unlock (&self->lock);
  for (guint i = 0; i < readers->len; i++)
    {
      GThread *t = g_ptr_array_index (readers, i);
      if (t == g_thread_self ())
        g_thread_unref (t);
      else
        g_thread_join (t);
    }
}

void
bro_event_feed_close (BroEventFeed *self)
{
  bro_event_stream_close (self->stream);
  g_mutex_lock (&self->lock);
  g_clear_pointer (&self->first, bro_engine_event_unref);
  g_mutex_unlock (&self->lock);
  join_readers (self);
}

gboolean
_bro_event_feed_is_closed (BroEventFeed *self)
{
  return bro_event_stream_is_closed (self->stream);
}

BroEventFeed *
_bro_event_feed_new (BroEngineExecutor *executor, BroEventStream *stream, const BroId *job_log, gsize n_job_log, BroEngineEvent *first,
                     BroSnapshotFunc snapshot, GObject *owner)
{
  BroEventFeed *self = g_object_new (BRO_TYPE_EVENT_FEED, NULL);
  self->executor = g_object_ref (executor);
  self->stream = stream;
  for (gsize i = 0; i < n_job_log; i++)
    g_hash_table_add (self->job_log, g_strdup (job_log[i].value));
  self->first = first;
  self->snapshot = snapshot;
  g_weak_ref_set (&self->owner, owner);
  return self;
}

static void
bro_event_feed_finalize (GObject *object)
{
  BroEventFeed *self = BRO_EVENT_FEED (object);
  bro_event_stream_close (self->stream);
  join_readers (self); /* every reader holds a reference, so they have all ended */
  g_ptr_array_unref (self->readers);
  g_clear_pointer (&self->first, bro_engine_event_unref);
  g_object_unref (self->stream);
  g_object_unref (self->executor);
  g_hash_table_unref (self->job_log);
  g_weak_ref_clear (&self->owner);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_event_feed_parent_class)->finalize (object);
}

static void
bro_event_feed_class_init (BroEventFeedClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_event_feed_finalize;
}

static void
bro_event_feed_init (BroEventFeed *self)
{
  g_mutex_init (&self->lock);
  self->readers = g_ptr_array_new ();
  self->job_log = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_weak_ref_init (&self->owner, NULL);
}
