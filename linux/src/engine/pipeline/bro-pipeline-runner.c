/* bro-pipeline-runner.c */
#include "bro-pipeline-runner.h"

#include "bro-acquire-output.h"
#include "bro-cancellation-source.h"
#include "bro-job-dto.h"
#include "bro-job-log.h"
#include "bro-job-outcome.h"
#include "bro-job-planner.h"
#include "bro-lease-snapshot.h"
#include "bro-log-line-dto.h"
#include "bro-message-code.h"
#include "bro-seal-output.h"
#include "bro-step-result.h"

#include <string.h>

/* ---- a decision a step waits on: shared by its worker and the executor ---- */

typedef struct {
  gatomicrefcount refs;
  GMutex lock;
  GCond cond;
  gboolean settled;
  char *answer;
  BroBroError *error;
} Waiter;

static Waiter *
waiter_new (void)
{
  Waiter *w = g_new0 (Waiter, 1);
  g_atomic_ref_count_init (&w->refs);
  g_mutex_init (&w->lock);
  g_cond_init (&w->cond);
  return w;
}

static Waiter *
waiter_ref (Waiter *w)
{
  g_atomic_ref_count_inc (&w->refs);
  return w;
}

static void
waiter_unref (Waiter *w)
{
  if (!w || !g_atomic_ref_count_dec (&w->refs))
    return;
  g_free (w->answer);
  bro_bro_error_free (w->error);
  g_mutex_clear (&w->lock);
  g_cond_clear (&w->cond);
  g_free (w);
}

/* Answers once: @answer, or @error (taken). */
static void
waiter_settle (Waiter *w, const char *answer, BroBroError *error)
{
  g_mutex_lock (&w->lock);
  if (!w->settled)
    {
      w->settled = TRUE;
      w->answer = g_strdup (answer);
      w->error = error;
      error = NULL;
      g_cond_broadcast (&w->cond);
    }
  g_mutex_unlock (&w->lock);
  bro_bro_error_free (error);
}

static BroBroError *
cancelled_error (void)
{
  return bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL, NULL);
}

static BroBroError *
stopping_error (void)
{
  return bro_bro_error_new (bro_message_code_wire (BRO_MSG_SERVER_STOPPING), NULL, NULL);
}

static BroBroError *
unexpected (const char *detail)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "detail", bro_json_value_new_string (detail));
  return bro_bro_error_new (bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), params, NULL);
}

/* ---- the runner and a job while it runs ---- */

struct _BroPipelineRunner {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroScheduler *scheduler;
  BroStore *store;
  BroStepRegistry *registry;
  BroStepEnvironment *environment;
  BroEngineClock *clock;
  BroCoalescer *coalescer;
  BroCoalescer *log_coalescer;
  GHashTable *live; /* job id → Live (a reference) */
  BroRunnerCrashFunc crash; /* nullable: the test roots' crash hook */
  gpointer crash_data;
  gboolean stopping; /* set by shutdown: nothing more is stored for any job */
  GPtrArray *idle;   /* Idle: who waits in shutdown for no job to be live */
};

typedef struct {
  BroRunnerIdle done;
  gpointer data;
  GDestroyNotify destroy;
} Idle;

static void
idle_free (gpointer data)
{
  Idle *i = data;
  if (i->destroy)
    i->destroy (i->data);
  g_free (i);
}

G_DEFINE_FINAL_TYPE (BroPipelineRunner, bro_pipeline_runner, G_TYPE_OBJECT)

#define LOG_INTERVAL 2.0 /* seconds */

typedef struct _Live Live;
static void gone (Live *l);
static void stopped (Live *l);

struct _Live {
  gatomicrefcount refs;
  BroPipelineRunner *runner; /* a reference */
  BroId id;
  BroJobRecord *record;
  BroCancellationSource *cancel;
  GArray *steps; /* BroStepKind */
  int seq;
  gboolean cancelled;
  BroProgressDto *progress;
  BroProgressDto *pending;
  BroBroMessage *blocked_by;
  BroDecisionDto *decision;
  Waiter *waiter;
  gint64 log_seq;
  GBytes *log_before; /* the log file as an earlier run (or recovery) left it; this run's lines follow */
  int warnings_before, errors_before; /* the warnings and errors in log_before, for the job's counts */
  GPtrArray *log;     /* BroLogLineDto */
  /* The log file as this run writes it, whole, from workers: the writes never overlap, and one that was overtaken by a
   * later one is dropped, so the file never goes back. */
  GMutex log_lock;
  guint log_written;
  gboolean directory_made;
  gboolean log_read;
  BroConfig *config;  /* read when the job starts */
  GPtrArray *outputs; /* BroStepOutput */
  GPtrArray *results; /* BroStepResult */
  GHashTable *checkpoints; /* BroStepKind → BroJsonValue (a reference): each kind's latest, from the rows, then this run's */
  BroLease *lease;
  BroInstant started;
  gboolean first_start; /* the job's first step is starting: its snapshot shows the record as it was before (no started_at) */
  BroStepReport *report;
  BroBroError *thrown;
  BroDecided *decided; /* how it ended, between conclude and the record's save */
  BroRunDone done;
  gpointer done_data;
  GDestroyNotify done_destroy;
};

static Live *
live_ref (Live *l)
{
  g_atomic_ref_count_inc (&l->refs);
  return l;
}

static void
live_unref (gpointer data)
{
  Live *l = data;
  if (!l || !g_atomic_ref_count_dec (&l->refs))
    return;
  if (l->done_destroy)
    l->done_destroy (l->done_data);
  bro_job_record_free (l->record);
  bro_cancellation_source_free (l->cancel);
  g_clear_pointer (&l->steps, g_array_unref);
  bro_progress_dto_free (l->progress);
  bro_progress_dto_free (l->pending);
  bro_bro_message_free (l->blocked_by);
  bro_decision_dto_free (l->decision);
  waiter_unref (l->waiter);
  g_clear_pointer (&l->log_before, g_bytes_unref);
  g_mutex_clear (&l->log_lock);
  g_ptr_array_unref (l->log);
  bro_config_free (l->config);
  g_ptr_array_unref (l->outputs);
  g_ptr_array_unref (l->results);
  g_hash_table_unref (l->checkpoints);
  bro_lease_unref (l->lease);
  bro_step_report_free (l->report);
  bro_bro_error_free (l->thrown);
  bro_decided_free (l->decided);
  g_object_unref (l->runner);
  g_free (l);
}

static char *
job_directory (BroPipelineRunner *self, const BroId *id)
{
  return g_strconcat (self->environment->data_directory, "/jobs/", id->value, NULL);
}

/* The job's counts (dtos.json Job.counts) once its log is read: the files it produced (Seal's, else Acquire's, as
 * JobService's job lists them), the read errors of its steps' outputs, and the warnings and errors in its log (an
 * earlier run's lines too). */
static BroJobTotals *
totals (Live *l)
{
  BroJobTotals *t = g_new0 (BroJobTotals, 1);
  const BroStepOutput *seal = NULL, *acquire = NULL;
  for (guint i = 0; i < l->outputs->len; i++)
    {
      const BroStepOutput *o = g_ptr_array_index (l->outputs, i);
      if (!seal && o->kind == BRO_STEP_KIND_SEAL && bro_json_value_member (o->data, "files"))
        seal = o;
      if (!acquire && o->kind == BRO_STEP_KIND_ACQUIRE && bro_json_value_member (o->data, "titles"))
        acquire = o;
      t->read_errors += o->read_errors;
    }
  if (seal)
    {
      g_autoptr (BroSealOutput) decoded = bro_seal_output_decode (seal->data);
      t->files = decoded ? (int) decoded->files->len : 0;
    }
  else if (acquire)
    {
      g_autoptr (BroAcquireOutput) decoded = bro_acquire_output_decode (acquire->data);
      g_autoptr (GPtrArray) produced = decoded ? bro_acquire_output_produced (decoded) : NULL;
      t->files = produced ? (int) produced->len : 0;
    }
  t->warnings = l->warnings_before;
  t->errors = l->errors_before;
  for (guint i = 0; i < l->log->len; i++)
    {
      const BroLogLineDto *line = g_ptr_array_index (l->log, i);
      t->warnings += line->severity == BRO_SEVERITY_WARNING;
      t->errors += line->severity == BRO_SEVERITY_ERROR;
    }
  return t;
}

static BroJobDto *
dto (Live *l)
{
  BroJobDto *d = bro_job_dto_of (l->record);
  d->progress = bro_progress_dto_copy (l->progress);
  d->blocked_by = l->blocked_by ? bro_bro_message_copy (l->blocked_by) : NULL;
  d->decision = bro_decision_dto_copy (l->decision);
  g_free (d->counts);
  d->counts = l->log_read ? totals (l) : NULL;
  return d;
}

static void
publish_job (BroPipelineRunner *self, BroEngineEventType type, Live *l)
{
  g_autoptr (BroJobDto) d = dto (l);
  g_autoptr (BroJsonValue) data = bro_json_value_new_object ();
  bro_json_value_set (data, "job", bro_job_dto_json (d));
  bro_engine_event_unref (bro_event_bus_publish (self->bus, type, data));
}

static void
set_state (Live *l, BroJobState state)
{
  l->record->state = state;
  publish_job (l->runner, BRO_ENGINE_EVENT_TYPE_JOB_CHANGED, l);
}

/* ---- workers: what the runner hands to the worker threads ---- */

typedef void (*Next) (Live *l);

typedef struct {
  Live *live;
  BroStore *store;
  BroJobRecord *record;     /* to save, or loaded */
  BroStepRecord *step;      /* to save */
  GPtrArray *found;         /* steps done */
  GPtrArray *rows;          /* every step's row */
  BroStep *run_step;
  BroStepContext *context;
  BroStepReport *report;
  BroFileSystem *fs;
  char *log_directory;
  GBytes *log;
  guint log_lines;
  Next next;
} Work;

static Work *
work_new (Live *l)
{
  Work *w = g_new0 (Work, 1);
  w->live = live_ref (l);
  w->store = g_object_ref (l->runner->store);
  return w;
}

static void
work_free (gpointer data)
{
  Work *w = data;
  live_unref (w->live);
  g_object_unref (w->store);
  bro_job_record_free (w->record);
  bro_step_record_free (w->step);
  g_clear_pointer (&w->found, g_ptr_array_unref);
  g_clear_pointer (&w->rows, g_ptr_array_unref);
  g_clear_object (&w->run_step);
  bro_step_context_free (w->context);
  bro_step_report_free (w->report);
  g_clear_object (&w->fs);
  g_free (w->log_directory);
  g_clear_pointer (&w->log, g_bytes_unref);
  g_free (w);
}

static void conclude (Live *l, BroBroError *fatal);
static void next_step (Live *l);

static void
concluded (Live *l)
{
  conclude (l, NULL);
}

/* Saves the record (and the step record, when set), then goes on with w->next. */
static gboolean
save_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  if (w->record && !bro_job_repository_update (bro_store_jobs (w->store), w->record, error))
    return FALSE;
  return !w->step || bro_step_repository_save (bro_store_steps (w->store), w->step, error);
}

static void
save_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  if (error)
    conclude (w->live, bro_bro_error_copy (error));
  else
    w->next (w->live);
}

/* The job's place in its queue as it is now, into its record before a save (JobService stores it when it changes; a
 * stale one here would undo that). */
static void
place (Live *l)
{
  int position = bro_scheduler_position (l->runner->scheduler, &l->id);
  if (position >= 0)
    l->record->position = position;
}

static void
save_record (Live *l, Next next)
{
  Work *w = work_new (l);
  place (l);
  w->record = bro_job_record_copy (l->record);
  w->next = next;
  bro_engine_executor_worker (l->runner->executor, save_work, save_done, w, work_free);
}

/* ---- context parts: a step's reports go back to the executor ---- */

typedef struct {
  Live *live;
  int seq;
  double fraction;
  BroBroMessage *message;
  char *current;
  Waiter *waiter;
  BroDecisionDto *decision;
  char *volume;
  char *path;
  gint64 need;
  gint64 free;
  BroStepKind kind;
  BroJsonValue *checkpoint;
} Posted;

static void
posted_free (gpointer data)
{
  Posted *p = data;
  live_unref (p->live);
  bro_bro_message_free (p->message);
  g_free (p->current);
  waiter_unref (p->waiter);
  bro_decision_dto_free (p->decision);
  g_free (p->volume);
  g_free (p->path);
  if (p->checkpoint)
    bro_json_value_unref (p->checkpoint);
  g_free (p);
}

static void
on_progress (gpointer data)
{
  Posted *p = data;
  Live *l = p->live;
  double f = CLAMP (p->fraction, 0, 1);
  if (l->seq != p->seq || l->record->state == BRO_JOB_STATE_FINISHED)
    return;
  bro_progress_dto_free (l->pending);
  l->pending = bro_progress_dto_new (g_array_index (l->steps, BroStepKind, p->seq), p->seq, l->steps->len,
                                     (p->seq + f) / l->steps->len, bro_bro_message_copy (p->message));
  l->pending->has_current_fraction = TRUE;
  l->pending->current_fraction = f;
  l->pending->current = g_strdup (p->current);
  {
    g_autoptr (BroJsonValue) none = bro_json_value_new_null ();
    bro_coalescer_offer (l->runner->coalescer, l->id.value, none);
  }
}

static void
on_log (gpointer data)
{
  Posted *p = data;
  Live *l = p->live;
  BroInstant now = bro_engine_clock_now (l->runner->clock);
  BroLogLineDto *line = bro_log_line_dto_new (++l->log_seq, now, p->message->severity, p->message->code, p->message->params);
  g_autoptr (BroJsonValue) data_json = bro_json_value_new_object ();
  BroJsonValue *lines = bro_json_value_new_array ();
  g_ptr_array_add (l->log, line);
  bro_json_value_append (lines, bro_log_line_dto_json (line));
  bro_json_value_set (data_json, "jobId", bro_json_value_new_string (l->id.value));
  bro_json_value_set (data_json, "lines", lines);
  bro_engine_event_unref (bro_event_bus_publish (l->runner->bus, BRO_ENGINE_EVENT_TYPE_JOB_LOG, data_json));
  {
    g_autoptr (BroJsonValue) none = bro_json_value_new_null ();
    bro_coalescer_offer (l->runner->log_coalescer, l->id.value, none);
  }
}

static void
on_ask (gpointer data)
{
  Posted *p = data;
  Live *l = p->live;
  if (bro_cancellation_token_is_cancelled (bro_cancellation_source_token (l->cancel)))
    {
      waiter_settle (p->waiter, NULL, cancelled_error ());
      return;
    }
  bro_decision_dto_free (l->decision);
  waiter_unref (l->waiter);
  l->decision = g_steal_pointer (&p->decision);
  l->waiter = waiter_ref (p->waiter);
  set_state (l, BRO_JOB_STATE_AWAITING_DECISION);
}

static void
on_reserve (gpointer data)
{
  Posted *p = data;
  BroBroError *error = NULL;
  if (bro_scheduler_reserve (p->live->runner->scheduler, &p->live->id, p->volume, p->path, p->need, p->free, &error))
    waiter_settle (p->waiter, "ok", NULL);
  else
    waiter_settle (p->waiter, NULL, error);
}

static void
on_checkpoint (gpointer data)
{
  Posted *p = data;
  g_hash_table_replace (p->live->checkpoints, GINT_TO_POINTER (p->kind), g_steal_pointer (&p->checkpoint));
}

#define TYPE_REPORTER (reporter_get_type ())
G_DECLARE_FINAL_TYPE (Reporter, reporter, RUNNER, REPORTER, GObject)

struct _Reporter {
  GObject parent_instance;
  Live *live;
  int seq;
  gboolean stores;      /* its checkpoints are stored (not the Release at the end) */
  BroStepKind kind;
  BroInstant started;
  BroJsonValue *saved;  /* nullable: what the step's last attempt saved */
};

static void reporter_progress_init (BroProgressReporterInterface *iface);
static void reporter_logger_init (BroJobLoggerInterface *iface);
static void reporter_decisions_init (BroDecisionPortInterface *iface);
static void reporter_space_init (BroSpaceReservationsInterface *iface);
static void reporter_checkpointer_init (BroCheckpointerInterface *iface);

/* One object is all five: the progress reporter, the logger, the decision port, the space reservations and the
 * checkpointer of a step. */
G_DEFINE_FINAL_TYPE_WITH_CODE (Reporter, reporter, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_PROGRESS_REPORTER, reporter_progress_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_JOB_LOGGER, reporter_logger_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DECISION_PORT, reporter_decisions_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_SPACE_RESERVATIONS, reporter_space_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CHECKPOINTER, reporter_checkpointer_init))

static Posted *
posted_new (Reporter *r)
{
  Posted *p = g_new0 (Posted, 1);
  p->live = live_ref (r->live);
  p->seq = r->seq;
  return p;
}

static void
reporter_report (BroProgressReporter *self, double fraction, const BroBroMessage *operation, const char *current)
{
  Reporter *r = (Reporter *) self;
  Posted *p = posted_new (r);
  p->fraction = fraction;
  p->message = bro_bro_message_copy (operation);
  p->current = g_strdup (current);
  bro_engine_executor_post (r->live->runner->executor, on_progress, p, posted_free);
}

static void
reporter_log (BroJobLogger *self, const BroBroMessage *message)
{
  Reporter *r = (Reporter *) self;
  Posted *p = posted_new (r);
  p->message = bro_bro_message_copy (message);
  bro_engine_executor_post (r->live->runner->executor, on_log, p, posted_free);
}

/* On the step's worker: waits for the executor's answer. */
static char *
reporter_ask (BroDecisionPort *self, const BroDecisionDto *decision, BroBroError **error)
{
  Reporter *r = (Reporter *) self;
  Waiter *w = waiter_new ();
  Posted *p = posted_new (r);
  char *answer;
  p->waiter = waiter_ref (w);
  p->decision = bro_decision_dto_copy (decision);
  if (!bro_engine_executor_post (r->live->runner->executor, on_ask, p, posted_free))
    waiter_settle (w, NULL, cancelled_error ());
  g_mutex_lock (&w->lock);
  while (!w->settled)
    g_cond_wait (&w->cond, &w->lock);
  g_mutex_unlock (&w->lock);
  answer = g_strdup (w->answer);
  if (!answer)
    *error = bro_bro_error_copy (w->error);
  waiter_unref (w);
  return answer;
}

/* On the step's worker: waits until the executor has asked the Scheduler. */
static gboolean
reporter_reserve (BroSpaceReservations *self, const char *volume, const char *path, gint64 need, gint64 free, BroBroError **error)
{
  Reporter *r = (Reporter *) self;
  Waiter *w = waiter_new ();
  Posted *p = posted_new (r);
  gboolean ok;
  p->waiter = waiter_ref (w);
  p->volume = g_strdup (volume);
  p->path = g_strdup (path);
  p->need = need;
  p->free = free;
  if (!bro_engine_executor_post (r->live->runner->executor, on_reserve, p, posted_free))
    waiter_settle (w, NULL, cancelled_error ());
  g_mutex_lock (&w->lock);
  while (!w->settled)
    g_cond_wait (&w->cond, &w->lock);
  g_mutex_unlock (&w->lock);
  ok = w->answer != NULL;
  if (!ok)
    *error = bro_bro_error_copy (w->error);
  waiter_unref (w);
  return ok;
}

static BroJsonValue *
reporter_saved (BroCheckpointer *self)
{
  Reporter *r = (Reporter *) self;
  return r->saved ? bro_json_value_ref (r->saved) : NULL;
}

/* On the step's worker: the executor hears of it first (posted before the store write, so it is there before the
 * step's result), then the step's running row is stored with it from this thread. */
static gboolean
reporter_save (BroCheckpointer *self, const BroJsonValue *checkpoint, BroBroError **error)
{
  Reporter *r = (Reporter *) self;
  Posted *p;
  g_autoptr (BroStepRecord) row = NULL;
  if (!r->stores)
    return TRUE;
  p = posted_new (r);
  p->kind = r->kind;
  p->checkpoint = bro_json_value_ref ((BroJsonValue *) checkpoint);
  if (!bro_engine_executor_post (r->live->runner->executor, on_checkpoint, p, posted_free))
    {
      *error = cancelled_error ();
      return FALSE;
    }
  row = bro_step_record_new ();
  row->job_id = r->live->id;
  row->seq = r->seq;
  row->kind = r->kind;
  row->state = BRO_STEP_STATE_RUNNING;
  row->attempt = 1;
  row->has_started_at = TRUE;
  row->started_at = r->started;
  row->checkpoint = bro_json_value_ref ((BroJsonValue *) checkpoint);
  return bro_step_repository_save (bro_store_steps (r->live->runner->store), row, error);
}

static void
reporter_checkpointer_init (BroCheckpointerInterface *iface)
{
  iface->saved = reporter_saved;
  iface->save = reporter_save;
}

static void
reporter_space_init (BroSpaceReservationsInterface *iface)
{
  iface->reserve = reporter_reserve;
}

static void
reporter_progress_init (BroProgressReporterInterface *iface)
{
  iface->report = reporter_report;
}

static void
reporter_logger_init (BroJobLoggerInterface *iface)
{
  iface->log = reporter_log;
}

static void
reporter_decisions_init (BroDecisionPortInterface *iface)
{
  iface->ask = reporter_ask;
}

static void
reporter_finalize (GObject *object)
{
  Reporter *r = (Reporter *) object;
  if (r->saved)
    bro_json_value_unref (r->saved);
  live_unref (r->live);
  G_OBJECT_CLASS (reporter_parent_class)->finalize (object);
}

static void
reporter_class_init (ReporterClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = reporter_finalize;
}

static void
reporter_init (Reporter *self)
{
}

/* ---- the loop ---- */

static BroJobSnapshot *
snapshot (Live *l)
{
  BroJobPlan *plan = bro_job_plan_copy (l->record->plan);
  BroJobSnapshot *s;
  if (plan)
    {
      g_array_set_size (plan->steps, 0);
      g_array_append_vals (plan->steps, l->steps->data, l->steps->len);
    }
  s = bro_job_snapshot_new (&l->id, bro_job_request_copy (l->record->request), plan);
  s->has_started_at = l->record->has_started_at && !l->first_start;
  s->started_at = s->has_started_at ? l->record->started_at : (BroInstant) { 0 };
  for (guint i = 0; i < l->outputs->len; i++)
    g_ptr_array_add (s->outputs, bro_step_output_copy (g_ptr_array_index (l->outputs, i)));
  return s;
}

static void
on_wait (const BroWait *wait, gpointer data)
{
  Live *l = data;
  g_clear_pointer (&l->blocked_by, bro_bro_message_free);
  if (wait->kind == BRO_WAIT_BLOCKED)
    {
      BroMessageCode code = BRO_MSG_INTERNAL_UNEXPECTED;
      bro_message_code_parse (wait->reason->code, &code);
      l->blocked_by = bro_bro_message_new (code, bro_json_value_ref (wait->reason->params), BRO_SEVERITY_INFO);
      set_state (l, BRO_JOB_STATE_BLOCKED);
    }
  else
    set_state (l, BRO_JOB_STATE_WAITING_FOR_RESOURCES);
}

static gboolean
step_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  w->report = bro_step_run (w->run_step, w->context, error);
  if (!w->report && !*error)
    *error = unexpected ("a step returned nothing");
  return w->report != NULL;
}

static gboolean
end_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  return bro_step_repository_save (bro_store_steps (w->store), w->step, error);
}

/* @w's copy of what the log file is to hold: what was there when the job started, then this run's lines. */
static void
log_snapshot (Live *l, Work *w)
{
  g_autoptr (GBytes) added = bro_job_log_encode (l->log);
  GByteArray *all = g_byte_array_new ();
  if (l->log_before)
    g_byte_array_append (all, g_bytes_get_data (l->log_before, NULL), g_bytes_get_size (l->log_before));
  g_byte_array_append (all, g_bytes_get_data (added, NULL), g_bytes_get_size (added));
  w->log = g_byte_array_free_to_bytes (all);
  w->log_lines = l->log->len;
  w->log_directory = job_directory (l->runner, &l->id);
  w->fs = g_object_ref (l->runner->environment->fs);
}

/* Writes the job's log file (w->log into w->log_directory), written whole after each step and, through emit_log, while
 * one runs. */
static gboolean
log_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  Live *l = w->live;
  g_autofree char *path = g_strconcat (w->log_directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
  gboolean ok = TRUE;
  g_mutex_lock (&l->log_lock);
  if (w->log_lines > l->log_written)
    {
      ok = l->directory_made || bro_file_system_create_directory (w->fs, w->log_directory, FALSE, error);
      l->directory_made = l->directory_made || ok;
      ok = ok && bro_file_system_write_atomically (w->fs, path, w->log, 0600, error);
      if (ok)
        l->log_written = w->log_lines;
    }
  g_mutex_unlock (&l->log_lock);
  return ok;
}

/* The crash hook at a point: FALSE when it failed, which stops the runner (nothing more is stored for any job) and ends
 * the run as a shutdown does. */
static gboolean
crash_point (BroPipelineRunner *self, const char *prefix, BroStepKind kind)
{
  g_autofree char *point = NULL;
  g_autoptr (BroBroError) error = NULL;
  if (!self->crash)
    return TRUE;
  point = g_strconcat (prefix, bro_step_kind_to_wire (kind), NULL);
  if (self->crash (point, self->crash_data, &error))
    return TRUE;
  self->stopping = TRUE;
  return FALSE;
}

static void after_step (Live *l, const BroStepRecord *rec);

static void
log_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  if (error)
    conclude (w->live, bro_bro_error_copy (error));
  else
    after_step (w->live, w->step);
}

static void
end_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  Live *l = w->live;
  BroPipelineRunner *self = l->runner;
  BroStepRecord *rec = w->step;
  Work *lw;
  if (error)
    {
      conclude (l, bro_bro_error_copy (error));
      return;
    }
  if (!crash_point (self, "afterStep:", rec->kind))
    {
      conclude (l, NULL);
      return;
    }
  {
    g_autoptr (BroJsonValue) event = bro_json_value_new_object ();
    bro_json_value_set (event, "job", bro_json_value_new_string (l->id.value));
    bro_json_value_set (event, "seq", bro_json_value_new_integer (rec->seq));
    bro_json_value_set (event, "kind", bro_json_value_new_string (bro_step_kind_to_wire (rec->kind)));
    bro_json_value_set (event, "state", bro_json_value_new_string (bro_step_state_to_wire (rec->state)));
    bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_STEP_FINISHED, event));
  }
  if (l->log->len == 0)
    {
      after_step (l, rec);
      return;
    }
  lw = work_new (l);
  lw->step = g_steal_pointer (&w->step);
  log_snapshot (l, lw);
  bro_engine_executor_worker (self->executor, log_work, log_done, lw, work_free);
}

/* The step's row is stored (and the log written): its result, then what comes next. */
static void
after_step (Live *l, const BroStepRecord *rec)
{
  BroPipelineRunner *self = l->runner;
  BroStepKind kind = rec->kind;
  BroStepResult *result;
  g_ptr_array_add (l->outputs, bro_step_output_copy (rec->output));
  result = bro_step_result_new (kind, rec->state);
  result->error = bro_bro_error_copy (rec->error);
  if (l->report)
    {
      result->read_errors = l->report->read_errors;
      result->quarantined = l->report->quarantined;
      result->notice = bro_makemkv_notice_copy (l->report->notice);
      result->skip = l->report->skip_job ? bro_bro_message_copy (l->report->skip_job) : NULL;
      result->affects_outcome = l->report->affects_outcome;
    }
  g_ptr_array_add (l->results, result);
  if (kind == BRO_STEP_KIND_RELEASE && rec->state == BRO_STEP_STATE_SUCCEEDED)
    {
      bro_scheduler_release_drives (self->scheduler, &l->id);
      bro_scheduler_move_to (self->scheduler, &l->id, BRO_QUEUE_PROCESSING);
      l->record->queue = BRO_QUEUE_PROCESSING;
    }
  if (rec->state == BRO_STEP_STATE_CANCELLED)
    {
      l->cancelled = TRUE;
      conclude (l, NULL);
      return;
    }
  if (rec->state == BRO_STEP_STATE_FAILED && !bro_job_outcome_after_commit (kind))
    {
      conclude (l, NULL);
      return;
    }
  l->seq++;
  if (l->report && (l->report->plan || l->report->append_steps->len > 0))
    {
      g_array_append_vals (l->steps, l->report->append_steps->data, l->report->append_steps->len);
      if (l->report->plan)
        {
          bro_job_plan_free (l->record->plan);
          l->record->plan = bro_job_plan_copy (l->report->plan);
          if (l->report->plan->mode)
            {
              g_free (l->record->mode);
              l->record->mode = g_strdup (l->report->plan->mode);
            }
        }
      g_array_set_size (l->record->plan->steps, 0);
      g_array_append_vals (l->record->plan->steps, l->steps->data, l->steps->len);
      save_record (l, l->report->skip_job ? concluded : next_step);
      return;
    }
  if (l->report && l->report->skip_job) /* the job is skipped: nothing after this step runs */
    {
      conclude (l, NULL);
      return;
    }
  next_step (l);
}

static void
step_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  Live *l = w->live;
  BroPipelineRunner *self = l->runner;
  BroStepKind kind = g_array_index (l->steps, BroStepKind, l->seq);
  const BroBroError *failed;
  BroStepState state;
  Work *end;
  if (self->stopping) /* the step ended after the shutdown: its row stays running, as a kill leaves it */
    {
      stopped (l);
      return;
    }
  bro_step_report_free (l->report);
  l->report = g_steal_pointer (&w->report);
  g_clear_pointer (&l->thrown, bro_bro_error_free);
  l->thrown = bro_bro_error_copy (error);
  bro_scheduler_release (self->scheduler, l->lease);
  g_clear_pointer (&l->lease, bro_lease_unref);
  bro_coalescer_flush (self->coalescer, l->id.value);
  failed = l->thrown ? l->thrown : l->report ? l->report->error : NULL;
  state = failed ? (bro_cancellation_token_is_cancelled (bro_cancellation_source_token (l->cancel)) ? BRO_STEP_STATE_CANCELLED
                                                                                                    : BRO_STEP_STATE_FAILED)
          : l->report->skipped ? BRO_STEP_STATE_SKIPPED : BRO_STEP_STATE_SUCCEEDED;
  end = work_new (l);
  end->step = bro_step_record_new ();
  end->step->job_id = l->id;
  end->step->seq = l->seq;
  end->step->kind = kind;
  end->step->state = state;
  end->step->attempt = 1;
  end->step->has_started_at = TRUE;
  end->step->started_at = l->started;
  end->step->has_finished_at = TRUE;
  end->step->finished_at = bro_engine_clock_now (self->clock);
  end->step->output = bro_step_output_new (kind, l->report ? bro_json_value_ref (l->report->data) : bro_json_value_new_object (),
                                           l->report && l->report->summary ? bro_bro_message_copy (l->report->summary) : NULL);
  end->step->output->read_errors = l->report ? l->report->read_errors : 0;
  end->step->error = bro_bro_error_copy (failed);
  {
    BroJsonValue *checkpoint = g_hash_table_lookup (l->checkpoints, GINT_TO_POINTER (kind));
    end->step->checkpoint = checkpoint ? bro_json_value_ref (checkpoint) : NULL;
  }
  bro_engine_executor_worker (self->executor, end_work, end_done, end, work_free);
}

static void
started (Live *l)
{
  BroPipelineRunner *self = l->runner;
  Work *w = work_new (l);
  Reporter *r = g_object_new (TYPE_REPORTER, NULL);
  g_autofree char *directory = job_directory (self, &l->id);
  BroStep *step = bro_step_registry_step (self->registry, g_array_index (l->steps, BroStepKind, l->seq));
  set_state (l, BRO_JOB_STATE_RUNNING);
  r->live = live_ref (l);
  r->seq = l->seq;
  r->stores = TRUE;
  r->kind = g_array_index (l->steps, BroStepKind, l->seq);
  r->started = l->started;
  {
    BroJsonValue *saved = g_hash_table_lookup (l->checkpoints, GINT_TO_POINTER (r->kind));
    r->saved = saved ? bro_json_value_ref (saved) : NULL;
  }
  w->run_step = g_object_ref (step);
  w->context = bro_step_context_new (snapshot (l), bro_cancellation_source_token (l->cancel), g_object_ref (BRO_PROGRESS_REPORTER (r)),
                                     g_object_ref (BRO_JOB_LOGGER (r)), g_object_ref (BRO_DECISION_PORT (r)),
                                     g_object_ref (BRO_SPACE_RESERVATIONS (r)), BRO_CHECKPOINTER (r), self->environment, l->config, directory);
  l->first_start = FALSE;
  bro_engine_executor_worker (self->executor, step_work, step_done, w, work_free);
}

static void
acquired (BroLease *lease, const BroBroError *error, gpointer data)
{
  Live *l = data;
  BroPipelineRunner *self = l->runner;
  Work *w;
  if (lease && self->stopping) /* granted after the shutdown (its cancel was still on its way): nothing is stored */
    {
      l->lease = lease;
      stopped (l);
      return;
    }
  if (!lease)
    {
      if (strcmp (error->code, bro_message_code_wire (BRO_MSG_JOB_CANCELLED)) == 0)
        {
          l->cancelled = TRUE;
          conclude (l, NULL);
        }
      else
        conclude (l, bro_bro_error_copy (error));
      return;
    }
  l->lease = lease;
  g_clear_pointer (&l->blocked_by, bro_bro_message_free);
  l->started = bro_engine_clock_now (self->clock);
  l->record->state = BRO_JOB_STATE_RUNNING;
  l->first_start = !l->record->has_started_at;
  if (!l->record->has_started_at)
    {
      l->record->has_started_at = TRUE;
      l->record->started_at = l->started;
    }
  w = work_new (l);
  place (l);
  w->record = bro_job_record_copy (l->record);
  w->step = bro_step_record_new ();
  w->step->job_id = l->id;
  w->step->seq = l->seq;
  w->step->kind = g_array_index (l->steps, BroStepKind, l->seq);
  w->step->state = BRO_STEP_STATE_RUNNING;
  w->step->attempt = 1;
  w->step->has_started_at = TRUE;
  w->step->started_at = l->started;
  {
    BroJsonValue *checkpoint = g_hash_table_lookup (l->checkpoints, GINT_TO_POINTER (w->step->kind));
    w->step->checkpoint = checkpoint ? bro_json_value_ref (checkpoint) : NULL;
  }
  w->next = started;
  bro_engine_executor_worker (self->executor, save_work, save_done, w, work_free);
}

static void
next_step (Live *l)
{
  BroPipelineRunner *self = l->runner;
  BroStepKind kind;
  BroStep *step;
  g_autoptr (BroJobSnapshot) snap = NULL;
  g_autoptr (BroResourceSet) resources = NULL;
  g_autoptr (BroBroError) error = NULL;
  if (l->seq >= (int) l->steps->len)
    {
      conclude (l, NULL);
      return;
    }
  if (bro_cancellation_token_is_cancelled (bro_cancellation_source_token (l->cancel))) /* between steps, or before the first */
    {
      l->cancelled = TRUE;
      conclude (l, NULL);
      return;
    }
  kind = g_array_index (l->steps, BroStepKind, l->seq);
  step = bro_step_registry_step (self->registry, kind);
  if (!step)
    {
      g_autofree char *detail = g_strconcat ("no step ", bro_step_kind_to_wire (kind), NULL);
      conclude (l, unexpected (detail));
      return;
    }
  if (!crash_point (self, "beforeStep:", kind))
    {
      conclude (l, NULL);
      return;
    }
  bro_coalescer_drop (self->coalescer, l->id.value); /* a new step's first progress goes out at once */
  bro_coalescer_drop (self->log_coalescer, l->id.value); /* and its first log line */
  bro_progress_dto_free (l->progress);
  l->progress = bro_progress_dto_new (kind, l->seq, l->steps->len, (double) l->seq / l->steps->len,
                                      bro_bro_message_new (BRO_MSG_JOB_OP_STARTING, NULL, BRO_SEVERITY_INFO));
  snap = snapshot (l);
  resources = bro_step_resources (step, snap);
  if (!bro_scheduler_acquire (self->scheduler, &l->id, resources, on_wait, bro_cancellation_source_token (l->cancel), acquired, live_ref (l),
                              live_unref, &error))
    conclude (l, g_steal_pointer (&error));
}

/* What an earlier run (or recovery) left: the steps done and the log file. */
static gboolean
completed_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  g_autofree char *log_file = g_strconcat (w->log_directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
  g_autoptr (GPtrArray) rows = NULL;
  if (bro_file_system_exists (w->fs, log_file) && !(w->log = bro_file_system_read (w->fs, log_file, error)))
    return FALSE;
  rows = bro_step_repository_load (bro_store_steps (w->store), w->live->id, error);
  if (!rows)
    return FALSE;
  w->found = bro_job_planner_done (rows);
  w->rows = g_ptr_array_ref (rows);
  return TRUE;
}

static void
completed_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  Live *l = w->live;
  if (error)
    {
      conclude (l, bro_bro_error_copy (error));
      return;
    }
  if (w->log)
    {
      gsize n;
      const char *bytes = g_bytes_get_data (w->log, &n);
      g_autoptr (GPtrArray) before = NULL;
      l->log_before = g_bytes_ref (w->log);
      for (gsize i = 0; i < n; i++)
        if (bytes[i] == '\n')
          l->log_seq++;
      l->directory_made = n > 0;
      before = bro_job_log_decode (w->log);
      for (guint i = 0; before && i < before->len; i++)
        {
          const BroLogLineDto *line = g_ptr_array_index (before, i);
          l->warnings_before += line->severity == BRO_SEVERITY_WARNING;
          l->errors_before += line->severity == BRO_SEVERITY_ERROR;
        }
    }
  l->log_read = TRUE;
  for (guint i = 0; i < w->rows->len; i++) /* in seq order */
    {
      BroStepRecord *row = g_ptr_array_index (w->rows, i);
      if (row->checkpoint)
        g_hash_table_replace (l->checkpoints, GINT_TO_POINTER (row->kind), bro_json_value_ref (row->checkpoint));
    }
  for (guint i = 0; i < w->found->len; i++)
    {
      BroStepRecord *s = g_ptr_array_index (w->found, i);
      BroStepResult *result = bro_step_result_new (s->kind, s->state);
      result->error = bro_bro_error_copy (s->error);
      result->read_errors = s->output ? s->output->read_errors : 0;
      g_ptr_array_add (l->outputs, s->output ? bro_step_output_copy (s->output) : bro_step_output_new (s->kind, bro_json_value_new_object (), NULL));
      g_ptr_array_add (l->results, result);
    }
  l->seq = w->found->len;
  bro_scheduler_submit (l->runner->scheduler, &l->id, l->record->queue);
  next_step (l);
}

static void
planned (Live *l)
{
  Work *w = work_new (l);
  l->steps = g_array_copy (l->record->plan->steps);
  l->config = l->runner->environment->config ? bro_config_source_current (l->runner->environment->config)
                                             : bro_config_new (bro_json_value_new_object (), NULL);
  w->fs = g_object_ref (l->runner->environment->fs);
  w->log_directory = job_directory (l->runner, &l->id);
  bro_engine_executor_worker (l->runner->executor, completed_work, completed_done, w, work_free);
}

static gboolean
load_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  w->record = bro_job_repository_load (bro_store_jobs (w->store), w->live->id, error);
  return w->record != NULL || *error == NULL;
}

static void
load_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  Live *l = w->live;
  if (error || !w->record)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      g_autoptr (BroBroError) missing = NULL;
      bro_json_value_set (params, "jobId", bro_json_value_new_string (l->id.value));
      missing = bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_NOT_FOUND), params, NULL);
      l->done (NULL, error ? error : missing, l->done_data);
      gone (l);
      return;
    }
  l->record = g_steal_pointer (&w->record);
  if (l->runner->stopping) /* loaded after the shutdown: nothing is stored, not even the plan */
    {
      stopped (l);
      return;
    }
  if (!l->record->plan)
    {
      g_autoptr (GArray) initial = bro_job_planner_initial (l->record->request);
      if (initial->len == 0)
        {
          g_autofree char *detail = g_strconcat ("no steps for a ", bro_job_kind_to_wire (l->record->kind), " job yet", NULL);
          l->steps = g_array_new (FALSE, FALSE, sizeof (BroStepKind));
          conclude (l, unexpected (detail));
          return;
        }
      l->record->plan = bro_job_plan_new ();
      g_array_append_vals (l->record->plan->steps, initial->data, initial->len);
      save_record (l, planned);
      return;
    }
  planned (l);
}

void
bro_pipeline_runner_run (BroPipelineRunner *self, const BroId *job_id, BroRunDone done, gpointer data, GDestroyNotify destroy)
{
  Live *l = g_new0 (Live, 1);
  Work *w;
  g_atomic_ref_count_init (&l->refs);
  l->runner = g_object_ref (self);
  l->id = *job_id;
  l->cancel = bro_cancellation_source_new ();
  l->seq = -1;
  l->log = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_line_dto_free);
  g_mutex_init (&l->log_lock);
  l->outputs = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_output_free);
  l->results = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_result_free);
  l->checkpoints = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_json_value_unref);
  l->done = done;
  l->done_data = data;
  l->done_destroy = destroy;
  g_hash_table_replace (self->live, g_strdup (job_id->value), l);
  w = work_new (l);
  bro_engine_executor_worker (self->executor, load_work, load_done, w, work_free);
}

/* ---- the end ---- */

static void
finish_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  Live *l = w->live;
  BroPipelineRunner *self = l->runner;
  /* The job has ended either way; what the store couldn't keep, recovery will see as unfinished. */
  bro_coalescer_drop (self->coalescer, l->id.value);
  bro_coalescer_drop (self->log_coalescer, l->id.value);
  bro_scheduler_finish (self->scheduler, &l->id);
  publish_job (self, BRO_ENGINE_EVENT_TYPE_JOB_FINISHED, l);
  {
    BroDecided d = { l->record->outcome, l->record->error };
    l->done (&d, NULL, l->done_data);
  }
  gone (l);
}

static gboolean
finish_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  g_autoptr (BroBroError) ignored = NULL;
  bro_job_repository_update (bro_store_jobs (w->store), w->record, &ignored);
  return TRUE;
}

static void record_end (Live *l);

static gboolean
release_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  g_autoptr (BroBroError) ignored = NULL;
  g_autoptr (BroStepReport) report = bro_step_run (w->run_step, w->context, &ignored);
  return TRUE; /* the job ends either way; the step logs a failed eject itself */
}

/* A write that fails here is left alone: the job ends, or the next write takes it up, and a step's end reports it. */
static gboolean
quiet_log_work (gpointer data, BroBroError **error)
{
  g_autoptr (BroBroError) ignored = NULL;
  log_work (data, &ignored);
  return TRUE;
}

static void
release_log_done (gpointer data, const BroBroError *error)
{
  record_end (((Work *) data)->live);
}

/* The Release step has run (its log lines are posted before this): writes the log, then records the end. */
static void
release_done (gpointer data, const BroBroError *error)
{
  Live *l = ((Work *) data)->live;
  BroPipelineRunner *self = l->runner;
  Work *w;
  if (l->log->len == 0)
    {
      record_end (l);
      return;
    }
  w = work_new (l);
  log_snapshot (l, w);
  bro_engine_executor_worker (self->executor, quiet_log_work, release_log_done, w, work_free);
}

/* A job that ends while it still holds its drive (it stopped before its Release step: failed, skipped, cancelled) runs
 * the Release step once more, unstored, with its outcome as the context's finishing. */
static gboolean
release_at_end (Live *l)
{
  BroPipelineRunner *self = l->runner;
  BroStep *step = bro_step_registry_step (self->registry, BRO_STEP_KIND_RELEASE);
  g_autoptr (BroLeaseSnapshot) leases = NULL;
  gboolean holds = FALSE;
  Reporter *r;
  Work *w;
  g_autofree char *directory = NULL;
  if (!l->record || !l->log_read || !l->steps || !step)
    return FALSE;
  leases = bro_scheduler_leases (self->scheduler);
  for (guint i = 0; i < leases->drives->len; i++)
    holds |= strcmp (((BroDriveHolder *) g_ptr_array_index (leases->drives, i))->job.value, l->id.value) == 0;
  if (!holds)
    return FALSE;
  r = g_object_new (TYPE_REPORTER, NULL);
  r->live = live_ref (l);
  r->seq = G_MININT; /* its progress is never shown */
  directory = job_directory (self, &l->id);
  w = work_new (l);
  w->run_step = g_object_ref (step);
  w->context = bro_step_context_new (snapshot (l), bro_cancellation_source_token (l->cancel), g_object_ref (BRO_PROGRESS_REPORTER (r)),
                                     g_object_ref (BRO_JOB_LOGGER (r)), g_object_ref (BRO_DECISION_PORT (r)),
                                     g_object_ref (BRO_SPACE_RESERVATIONS (r)), BRO_CHECKPOINTER (r), self->environment, l->config, directory);
  w->context->finishing = g_new0 (BroDecided, 1);
  w->context->finishing->outcome = l->decided->outcome;
  w->context->finishing->error = bro_bro_error_copy (l->decided->error);
  bro_engine_executor_worker (self->executor, release_work, release_done, w, work_free);
  return TRUE;
}

/* Decides the outcome (or fails with @fatal, taken), runs the Release step at the end when the job still holds its
 * drive, records the outcome, gives everything back, says so. */
static void
conclude (Live *l, BroBroError *fatal)
{
  BroDecided *decided;
  if (l->runner->stopping)
    {
      bro_bro_error_free (fatal);
      stopped (l);
      return;
    }
  if (fatal)
    {
      decided = g_new0 (BroDecided, 1);
      decided->outcome = BRO_OUTCOME_FAILED;
      decided->error = fatal;
    }
  else
    decided = bro_job_outcome_decide (l->results, l->cancelled || bro_cancellation_token_is_cancelled (bro_cancellation_source_token (l->cancel)),
                                      NULL);
  bro_decided_free (l->decided);
  l->decided = decided;
  if (!release_at_end (l))
    record_end (l);
}

static void
record_end (Live *l)
{
  BroPipelineRunner *self = l->runner;
  const BroDecided *decided = l->decided;
  Work *w;
  if (self->stopping) /* the Release at the end ran across the shutdown */
    {
      stopped (l);
      return;
    }
  g_clear_pointer (&l->progress, bro_progress_dto_free);
  g_clear_pointer (&l->pending, bro_progress_dto_free);
  g_clear_pointer (&l->blocked_by, bro_bro_message_free);
  g_clear_pointer (&l->decision, bro_decision_dto_free);
  l->record->state = BRO_JOB_STATE_FINISHED;
  l->record->has_outcome = TRUE;
  l->record->outcome = decided->outcome;
  bro_bro_error_free (l->record->error);
  l->record->error = bro_bro_error_copy (decided->error);
  l->record->has_finished_at = TRUE;
  l->record->finished_at = bro_engine_clock_now (self->clock);
  w = work_new (l);
  w->record = bro_job_record_copy (l->record);
  bro_engine_executor_worker (self->executor, finish_work, finish_done, w, work_free);
}

/* The job leaves the runner; the last one out after a shutdown lets it return. */
static void
gone (Live *l)
{
  BroPipelineRunner *self = l->runner;
  g_hash_table_remove (self->live, l->id.value);
  if (self->stopping && g_hash_table_size (self->live) == 0)
    {
      g_autoptr (GPtrArray) waiting = g_steal_pointer (&self->idle);
      self->idle = g_ptr_array_new_with_free_func (idle_free);
      for (guint i = 0; i < waiting->len; i++)
        {
          Idle *idle = g_ptr_array_index (waiting, i);
          idle->done (idle->data);
        }
    }
}

/* After the shutdown: nothing is stored or announced; the run ends with server.stopping. */
static void
stopped (Live *l)
{
  g_autoptr (BroBroError) error = stopping_error ();
  bro_coalescer_drop (l->runner->coalescer, l->id.value);
  bro_coalescer_drop (l->runner->log_coalescer, l->id.value);
  l->done (NULL, error, l->done_data);
  gone (l);
}

/* ---- commands ---- */

void
bro_pipeline_runner_shutdown (BroPipelineRunner *self, BroRunnerIdle done, gpointer data, GDestroyNotify destroy)
{
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  gpointer key;
  self->stopping = TRUE;
  g_hash_table_iter_init (&iter, self->live);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (ids, g_strdup (key));
  for (guint i = 0; i < ids->len; i++)
    {
      BroId id;
      g_strlcpy (id.value, g_ptr_array_index (ids, i), sizeof id.value);
      bro_pipeline_runner_cancel (self, &id);
    }
  if (g_hash_table_size (self->live) == 0)
    {
      done (data);
      if (destroy)
        destroy (data);
      return;
    }
  {
    Idle *idle = g_new0 (Idle, 1);
    idle->done = done;
    idle->data = data;
    idle->destroy = destroy;
    g_ptr_array_add (self->idle, idle);
  }
}

void
bro_pipeline_runner_cancel (BroPipelineRunner *self, const BroId *job_id)
{
  Live *l = g_hash_table_lookup (self->live, job_id->value);
  if (!l)
    return;
  bro_cancellation_source_cancel (l->cancel);
  if (l->decision)
    {
      g_clear_pointer (&l->decision, bro_decision_dto_free);
      waiter_settle (l->waiter, NULL, cancelled_error ());
      g_clear_pointer (&l->waiter, waiter_unref);
    }
}

gboolean
bro_pipeline_runner_decide (BroPipelineRunner *self, const BroId *job_id, const char *answer, BroBroError **error)
{
  Live *l = g_hash_table_lookup (self->live, job_id->value);
  gboolean offered = FALSE;
  BroJsonValue *params;
  if (!l || !l->decision)
    {
      params = bro_json_value_new_object ();
      bro_json_value_set (params, "jobId", bro_json_value_new_string (job_id->value));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_NOT_AWAITING_DECISION), params);
      return FALSE;
    }
  for (guint i = 0; i < l->decision->answers->len; i++)
    offered |= strcmp (((BroDecisionAnswer *) g_ptr_array_index (l->decision->answers, i))->id, answer) == 0;
  if (!offered)
    {
      g_autofree char *detail = g_strconcat ("answer ", answer, NULL);
      params = bro_json_value_new_object ();
      bro_json_value_set (params, "detail", bro_json_value_new_string (detail));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_SERVER_BAD_REQUEST), params);
      return FALSE;
    }
  g_clear_pointer (&l->decision, bro_decision_dto_free);
  set_state (l, BRO_JOB_STATE_RUNNING);
  waiter_settle (l->waiter, answer, NULL);
  g_clear_pointer (&l->waiter, waiter_unref);
  return TRUE;
}

BroJobDto *
bro_pipeline_runner_job (BroPipelineRunner *self, const BroId *job_id)
{
  Live *l = g_hash_table_lookup (self->live, job_id->value);
  return l && l->record ? dto (l) : NULL;
}

GPtrArray *
bro_pipeline_runner_log (BroPipelineRunner *self, const BroId *job_id)
{
  Live *l = g_hash_table_lookup (self->live, job_id->value);
  GPtrArray *lines;
  if (!l || !l->log_read)
    return NULL;
  lines = bro_job_log_decode (l->log_before);
  for (guint i = 0; i < l->log->len; i++)
    {
      BroLogLineDto *line = g_ptr_array_index (l->log, i);
      g_ptr_array_add (lines, bro_log_line_dto_new (line->seq, line->t, line->severity, line->code, line->params));
    }
  return lines;
}

/* ---- progress ---- */

static void
emit_progress (const char *key, BroJsonValue *value, gpointer data)
{
  BroPipelineRunner *self = data;
  Live *l = g_hash_table_lookup (self->live, key);
  if (!l || !l->pending)
    return;
  bro_progress_dto_free (l->progress);
  l->progress = g_steal_pointer (&l->pending);
  set_state (l, l->record->state);
}

/* ---- the log file while a step runs ---- */

static void
quiet_log_done (gpointer data, const BroBroError *error)
{
}

/* A running step's lines reach the file too, a step's first at once and later ones every LOG_INTERVAL, so a crash in a
 * long Acquire keeps them. The file is rewritten each time, so not per line. Never under Seal or KeepApart, which copy
 * it into the unit: on Windows a file being replaced can read as not there, and they would take that for a job without
 * a log. */
static void
emit_log (const char *key, BroJsonValue *value, gpointer data)
{
  BroPipelineRunner *self = data;
  Live *l = g_hash_table_lookup (self->live, key);
  BroStepKind kind;
  Work *w;
  if (self->stopping || !l)
    return;
  kind = g_array_index (l->steps, BroStepKind, l->seq);
  if (kind == BRO_STEP_KIND_SEAL || kind == BRO_STEP_KIND_KEEP_APART)
    return;
  w = work_new (l);
  log_snapshot (l, w);
  bro_engine_executor_worker (self->executor, quiet_log_work, quiet_log_done, w, work_free);
}

/* ---- the object ---- */

BroPipelineRunner *
bro_pipeline_runner_new (BroEngineExecutor *executor, BroEventBus *bus, BroScheduler *scheduler, BroStore *store, BroStepRegistry *registry,
                         const BroStepEnvironment *environment, BroEngineClock *clock, BroRunnerCrashFunc crash, gpointer crash_data)
{
  BroPipelineRunner *self = g_object_new (BRO_TYPE_PIPELINE_RUNNER, NULL);
  self->crash = crash;
  self->crash_data = crash_data;
  BroDuration interval = { 0.1 };
  BroDuration log_interval = { LOG_INTERVAL };
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->scheduler = g_object_ref (scheduler);
  self->store = g_object_ref (store);
  self->registry = g_object_ref (registry);
  self->environment = bro_step_environment_new (environment->tools, environment->fs, environment->clock, environment->drives,
                                                environment->library, environment->catalog, environment->config, environment->drive_lookup,
                                                environment->makemkv_setup, environment->data_directory, environment->messages,
                                                environment->app_version);
  self->clock = g_object_ref (clock);
  self->coalescer = bro_coalescer_new (clock, interval, emit_progress, self, NULL);
  self->log_coalescer = bro_coalescer_new (clock, log_interval, emit_log, self, NULL);
  return self;
}

static void
bro_pipeline_runner_finalize (GObject *object)
{
  BroPipelineRunner *self = BRO_PIPELINE_RUNNER (object);
  g_hash_table_unref (self->live);
  g_ptr_array_unref (self->idle);
  g_object_unref (self->coalescer);
  g_object_unref (self->log_coalescer);
  g_object_unref (self->executor);
  g_object_unref (self->bus);
  g_object_unref (self->scheduler);
  g_object_unref (self->store);
  g_object_unref (self->registry);
  bro_step_environment_free (self->environment);
  g_object_unref (self->clock);
  G_OBJECT_CLASS (bro_pipeline_runner_parent_class)->finalize (object);
}

static void
bro_pipeline_runner_class_init (BroPipelineRunnerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_pipeline_runner_finalize;
}

static void
bro_pipeline_runner_init (BroPipelineRunner *self)
{
  self->live = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, live_unref);
  self->idle = g_ptr_array_new_with_free_func (idle_free);
}
