/* bro-pipeline-runner.c */
#include "bro-pipeline-runner.h"

#include "bro-cancellation-source.h"
#include "bro-job-dto.h"
#include "bro-job-log.h"
#include "bro-job-outcome.h"
#include "bro-job-planner.h"
#include "bro-log-line-dto.h"
#include "bro-message-code.h"
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
  GHashTable *live; /* job id → Live (a reference) */
};

G_DEFINE_FINAL_TYPE (BroPipelineRunner, bro_pipeline_runner, G_TYPE_OBJECT)

typedef struct {
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
  GPtrArray *log;     /* BroLogLineDto */
  gboolean directory_made;
  BroConfig *config;  /* read when the job starts */
  GPtrArray *outputs; /* BroStepOutput */
  GPtrArray *results; /* BroStepResult */
  BroLease *lease;
  BroInstant started;
  BroStepReport *report;
  BroBroError *thrown;
  BroRunDone done;
  gpointer done_data;
  GDestroyNotify done_destroy;
} Live;

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
  g_ptr_array_unref (l->log);
  bro_config_free (l->config);
  g_ptr_array_unref (l->outputs);
  g_ptr_array_unref (l->results);
  bro_lease_unref (l->lease);
  bro_step_report_free (l->report);
  bro_bro_error_free (l->thrown);
  g_object_unref (l->runner);
  g_free (l);
}

static char *
job_directory (BroPipelineRunner *self, const BroId *id)
{
  return g_strconcat (self->environment->data_directory, "/jobs/", id->value, NULL);
}

static BroJobDto *
dto (Live *l)
{
  BroJobDto *d = bro_job_dto_of (l->record);
  d->progress = bro_progress_dto_copy (l->progress);
  d->blocked_by = l->blocked_by ? bro_bro_message_copy (l->blocked_by) : NULL;
  d->decision = bro_decision_dto_copy (l->decision);
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
  BroStep *run_step;
  BroStepContext *context;
  BroStepReport *report;
  BroFileSystem *fs;
  char *log_directory;
  GBytes *log;
  gboolean make_directory;
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

static void
save_record (Live *l, Next next)
{
  Work *w = work_new (l);
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

#define TYPE_REPORTER (reporter_get_type ())
G_DECLARE_FINAL_TYPE (Reporter, reporter, RUNNER, REPORTER, GObject)

struct _Reporter {
  GObject parent_instance;
  Live *live;
  int seq;
};

static void reporter_progress_init (BroProgressReporterInterface *iface);
static void reporter_logger_init (BroJobLoggerInterface *iface);
static void reporter_decisions_init (BroDecisionPortInterface *iface);

/* One object is all three: the progress reporter, the logger and the decision port of a step. */
G_DEFINE_FINAL_TYPE_WITH_CODE (Reporter, reporter, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_PROGRESS_REPORTER, reporter_progress_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_JOB_LOGGER, reporter_logger_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DECISION_PORT, reporter_decisions_init))

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
  live_unref (((Reporter *) object)->live);
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
  if (!bro_step_repository_save (bro_store_steps (w->store), w->step, error))
    return FALSE;
  if (w->log)
    {
      g_autofree char *path = g_strconcat (w->log_directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
      if (w->make_directory && !bro_file_system_create_directory (w->fs, w->log_directory, FALSE, error))
        return FALSE;
      if (!bro_file_system_write_atomically (w->fs, path, w->log, 0600, error))
        return FALSE;
    }
  return TRUE;
}

static void
end_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  Live *l = w->live;
  BroPipelineRunner *self = l->runner;
  BroStepRecord *rec = w->step;
  BroStepKind kind = rec->kind;
  BroStepResult *result;
  if (error)
    {
      conclude (l, bro_bro_error_copy (error));
      return;
    }
  {
    g_autoptr (BroJsonValue) event = bro_json_value_new_object ();
    bro_json_value_set (event, "job", bro_json_value_new_string (l->id.value));
    bro_json_value_set (event, "seq", bro_json_value_new_integer (rec->seq));
    bro_json_value_set (event, "kind", bro_json_value_new_string (bro_step_kind_to_wire (kind)));
    bro_json_value_set (event, "state", bro_json_value_new_string (bro_step_state_to_wire (rec->state)));
    bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_STEP_FINISHED, event));
  }
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
  if (l->report && l->report->append_steps->len > 0)
    {
      g_array_append_vals (l->steps, l->report->append_steps->data, l->report->append_steps->len);
      g_array_set_size (l->record->plan->steps, 0);
      g_array_append_vals (l->record->plan->steps, l->steps->data, l->steps->len);
      save_record (l, next_step);
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
  end->step->error = bro_bro_error_copy (failed);
  if (l->log->len > 0)
    {
      g_autoptr (GBytes) added = bro_job_log_encode (l->log);
      GByteArray *all = g_byte_array_new ();
      if (l->log_before)
        g_byte_array_append (all, g_bytes_get_data (l->log_before, NULL), g_bytes_get_size (l->log_before));
      g_byte_array_append (all, g_bytes_get_data (added, NULL), g_bytes_get_size (added));
      end->log = g_byte_array_free_to_bytes (all);
      end->log_directory = job_directory (self, &l->id);
      end->fs = g_object_ref (self->environment->fs);
      end->make_directory = !l->directory_made;
      l->directory_made = TRUE;
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
  w->run_step = g_object_ref (step);
  w->context = bro_step_context_new (snapshot (l), bro_cancellation_source_token (l->cancel), g_object_ref (BRO_PROGRESS_REPORTER (r)),
                                     g_object_ref (BRO_JOB_LOGGER (r)), BRO_DECISION_PORT (r), self->environment, l->config, directory);
  bro_engine_executor_worker (self->executor, step_work, step_done, w, work_free);
}

static void
acquired (BroLease *lease, const BroBroError *error, gpointer data)
{
  Live *l = data;
  BroPipelineRunner *self = l->runner;
  Work *w;
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
  if (!l->record->has_started_at)
    {
      l->record->has_started_at = TRUE;
      l->record->started_at = l->started;
    }
  w = work_new (l);
  w->record = bro_job_record_copy (l->record);
  w->step = bro_step_record_new ();
  w->step->job_id = l->id;
  w->step->seq = l->seq;
  w->step->kind = g_array_index (l->steps, BroStepKind, l->seq);
  w->step->state = BRO_STEP_STATE_RUNNING;
  w->step->attempt = 1;
  w->step->has_started_at = TRUE;
  w->step->started_at = l->started;
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
  kind = g_array_index (l->steps, BroStepKind, l->seq);
  step = bro_step_registry_step (self->registry, kind);
  if (!step)
    {
      g_autofree char *detail = g_strconcat ("no step ", bro_step_kind_to_wire (kind), NULL);
      conclude (l, unexpected (detail));
      return;
    }
  bro_coalescer_drop (self->coalescer, l->id.value); /* a new step's first progress goes out at once */
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
      l->log_before = g_bytes_ref (w->log);
      for (gsize i = 0; i < n; i++)
        if (bytes[i] == '\n')
          l->log_seq++;
      l->directory_made = n > 0;
    }
  for (guint i = 0; i < w->found->len; i++)
    {
      BroStepRecord *s = g_ptr_array_index (w->found, i);
      BroStepResult *result = bro_step_result_new (s->kind, s->state);
      result->error = bro_bro_error_copy (s->error);
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
      g_hash_table_remove (l->runner->live, l->id.value);
      return;
    }
  l->record = g_steal_pointer (&w->record);
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
  l->outputs = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_output_free);
  l->results = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_result_free);
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
  bro_scheduler_finish (self->scheduler, &l->id);
  publish_job (self, BRO_ENGINE_EVENT_TYPE_JOB_FINISHED, l);
  {
    BroDecided d = { l->record->outcome, l->record->error };
    l->done (&d, NULL, l->done_data);
  }
  g_hash_table_remove (self->live, l->id.value);
}

static gboolean
finish_work (gpointer data, BroBroError **error)
{
  Work *w = data;
  g_autoptr (BroBroError) ignored = NULL;
  bro_job_repository_update (bro_store_jobs (w->store), w->record, &ignored);
  return TRUE;
}

/* Decides the outcome (or fails with @fatal, taken), records it, gives everything back, says so. */
static void
conclude (Live *l, BroBroError *fatal)
{
  BroPipelineRunner *self = l->runner;
  g_autoptr (BroDecided) decided = NULL;
  Work *w;
  if (fatal)
    {
      decided = g_new0 (BroDecided, 1);
      decided->outcome = BRO_OUTCOME_FAILED;
      decided->error = fatal;
    }
  else
    decided = bro_job_outcome_decide (l->results, l->cancelled || bro_cancellation_token_is_cancelled (bro_cancellation_source_token (l->cancel)),
                                      NULL);
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

/* ---- commands ---- */

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

/* ---- the object ---- */

BroPipelineRunner *
bro_pipeline_runner_new (BroEngineExecutor *executor, BroEventBus *bus, BroScheduler *scheduler, BroStore *store, BroStepRegistry *registry,
                         const BroStepEnvironment *environment, BroEngineClock *clock)
{
  BroPipelineRunner *self = g_object_new (BRO_TYPE_PIPELINE_RUNNER, NULL);
  BroDuration interval = { 0.1 };
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->scheduler = g_object_ref (scheduler);
  self->store = g_object_ref (store);
  self->registry = g_object_ref (registry);
  self->environment = bro_step_environment_new (environment->tools, environment->fs, environment->clock, environment->drives,
                                                environment->library, environment->catalog, environment->config, environment->drive_lookup,
                                                environment->makemkv_setup, environment->data_directory);
  self->clock = g_object_ref (clock);
  self->coalescer = bro_coalescer_new (clock, interval, emit_progress, self, NULL);
  return self;
}

static void
bro_pipeline_runner_finalize (GObject *object)
{
  BroPipelineRunner *self = BRO_PIPELINE_RUNNER (object);
  g_hash_table_unref (self->live);
  g_object_unref (self->coalescer);
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
}
