/* bro-recovery-service.c */
#include "bro-recovery-service.h"

#include "bro-archive-files.h"
#include "bro-job-dto.h"
#include "bro-job-log.h"
#include "bro-job-outcome.h"
#include "bro-job-planner.h"
#include "bro-keep-apart-step.h"
#include "bro-message-code.h"
#include "bro-notes.h"
#include "bro-plan-output.h"
#include "bro-sanitizer.h"
#include "bro-seal-step-private.h"
#include "bro-step-result.h"
#include <string.h>

#define PATIENCE_MS 30000

struct _BroRecoveryService {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroProblemService *problems;
  BroStore *store;
  BroStepRegistry *registry;
  BroLibraryService *library;
  BroConfigSource *config;
  BroEngineClock *clock;
  BroFileSystem *fs;
  BroSettingsIsolation *isolation;
  BroMessageRenderer *renderer;
  char *data;
  GMutex log_lock; /* one append at a time: libraries are tidied side by side */
};

G_DEFINE_FINAL_TYPE (BroRecoveryService, bro_recovery_service, G_TYPE_OBJECT)

/* What the jobs' worker leaves for the executor, in order: an event to publish, or a problem to raise. */
typedef struct {
  BroEngineEventType type;
  BroJsonValue *data;     /* the event's, when no problem */
  BroBroMessage *problem; /* nullable */
} Publication;

static void
publication_free (gpointer data)
{
  Publication *p = data;
  g_clear_pointer (&p->data, bro_json_value_unref);
  g_clear_pointer (&p->problem, bro_bro_message_free);
  g_free (p);
}

/* One recover(): shared by its workers and its timer. */
typedef struct {
  gatomicrefcount refs;
  BroRecoveryService *self; /* a reference */
  BroRecoveryDone done;
  gpointer done_data;
  GDestroyNotify done_destroy;
  gboolean finished;
  int pending; /* library and data-folder workers still out */
  BroEngineTimer *timer;
  guint count; /* unfinished jobs found */
  GPtrArray *publications;
  GHashTable *unfinished; /* job id → itself; written by the jobs' worker only, read after */
} Run;

static Run *
run_ref (Run *r)
{
  g_atomic_ref_count_inc (&r->refs);
  return r;
}

static void
run_unref (gpointer data)
{
  Run *r = data;
  if (!g_atomic_ref_count_dec (&r->refs))
    return;
  if (r->done_destroy)
    r->done_destroy (r->done_data);
  g_clear_object (&r->timer);
  g_ptr_array_unref (r->publications);
  g_hash_table_unref (r->unfinished);
  g_object_unref (r->self);
  g_free (r);
}

static void
bro_recovery_service_finalize (GObject *object)
{
  BroRecoveryService *self = BRO_RECOVERY_SERVICE (object);
  g_object_unref (self->executor);
  g_object_unref (self->bus);
  g_object_unref (self->problems);
  g_object_unref (self->store);
  g_object_unref (self->registry);
  g_object_unref (self->library);
  g_object_unref (self->config);
  g_object_unref (self->clock);
  g_object_unref (self->fs);
  g_object_unref (self->isolation);
  g_object_unref (self->renderer);
  g_free (self->data);
  g_mutex_clear (&self->log_lock);
  G_OBJECT_CLASS (bro_recovery_service_parent_class)->finalize (object);
}

static void
bro_recovery_service_class_init (BroRecoveryServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_recovery_service_finalize;
}

static void
bro_recovery_service_init (BroRecoveryService *self)
{
  g_mutex_init (&self->log_lock);
}

BroRecoveryService *
bro_recovery_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroStore *store, BroStepRegistry *registry,
                          BroLibraryService *library, BroConfigSource *config, BroEngineClock *clock, BroFileSystem *fs,
                          BroSettingsIsolation *isolation, BroMessageRenderer *renderer, const char *data_directory)
{
  BroRecoveryService *self = g_object_new (BRO_TYPE_RECOVERY_SERVICE, NULL);
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->problems = g_object_ref (problems);
  self->store = g_object_ref (store);
  self->registry = g_object_ref (registry);
  self->library = g_object_ref (library);
  self->config = g_object_ref (config);
  self->clock = g_object_ref (clock);
  self->fs = g_object_ref (fs);
  self->isolation = g_object_ref (isolation);
  self->renderer = g_object_ref (renderer);
  self->data = g_strdup (data_directory);
  return self;
}

static char *
job_directory (BroRecoveryService *self, const BroId *id)
{
  return g_strconcat (self->data, "/jobs/", id->value, NULL);
}

static BroBroMessage *
message (BroMessageCode code, BroSeverity severity, const char *key, const char *value)
{
  BroJsonValue *params = bro_json_value_new_object ();
  if (key)
    bro_json_value_set (params, key, bro_json_value_new_string (value));
  return bro_bro_message_new (code, params, severity);
}

/* Appends the line to the job's log.jsonl (on a worker). */
static gboolean
log_line (BroRecoveryService *self, const BroId *job_id, const BroBroMessage *m, BroBroError **error)
{
  g_autofree char *directory = job_directory (self, job_id);
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_line_dto_free);
  gboolean ok;
  g_ptr_array_add (lines, bro_log_line_dto_new (0, bro_engine_clock_now (self->clock), m->severity, m->code, m->params));
  g_mutex_lock (&self->log_lock);
  ok = bro_job_log_append (self->fs, directory, lines, error);
  g_mutex_unlock (&self->log_lock);
  return ok;
}

/* The job's event into @publications (published on the executor). */
static void
publish_later (GPtrArray *publications, BroEngineEventType type, const BroJobRecord *record)
{
  Publication *p = g_new0 (Publication, 1);
  g_autoptr (BroJobDto) dto = bro_job_dto_of (record);
  p->type = type;
  p->data = bro_json_value_new_object ();
  bro_json_value_set (p->data, "job", bro_job_dto_json (dto));
  g_ptr_array_add (publications, p);
}

/* ---- jobs (one worker) ---- */

static gboolean
save_step (BroRecoveryService *self, const BroStepRecord *row, BroStepState state, BroBroError **error)
{
  g_autoptr (BroStepRecord) copy = bro_step_record_copy (row);
  copy->state = state;
  return bro_step_repository_save (bro_store_steps (self->store), copy, error);
}

static gboolean
requeue (BroRecoveryService *self, Run *r, const BroJobRecord *job, BroQueue queue, BroBroMessage *line, BroBroError **error)
{
  g_autoptr (BroJobRecord) record = bro_job_record_copy (job);
  g_autoptr (BroBroMessage) owned = line;
  record->state = BRO_JOB_STATE_QUEUED;
  record->queue = queue;
  g_clear_pointer (&record->decision, bro_json_value_unref);
  g_clear_pointer (&record->blocked_by, bro_json_value_unref);
  if (!bro_job_repository_update (bro_store_jobs (self->store), record, error) || !log_line (self, &job->id, owned, error))
    return FALSE;
  g_hash_table_add (r->unfinished, g_strdup (job->id.value));
  publish_later (r->publications, BRO_ENGINE_EVENT_TYPE_JOB_CHANGED, record);
  return TRUE;
}

/* Takes @decided and @line (nullable). */
static gboolean
finish (BroRecoveryService *self, Run *r, const BroJobRecord *job, BroDecided *decided, BroBroMessage *line, BroBroError **error)
{
  g_autoptr (BroJobRecord) record = bro_job_record_copy (job);
  g_autoptr (BroDecided) owned = decided;
  g_autoptr (BroBroMessage) owned_line = line;
  record->state = BRO_JOB_STATE_FINISHED;
  record->has_outcome = TRUE;
  record->outcome = decided->outcome;
  bro_bro_error_free (record->error);
  record->error = bro_bro_error_copy (decided->error);
  g_clear_pointer (&record->decision, bro_json_value_unref);
  g_clear_pointer (&record->blocked_by, bro_json_value_unref);
  record->has_finished_at = TRUE;
  record->finished_at = bro_engine_clock_now (self->clock);
  if (!bro_job_repository_update (bro_store_jobs (self->store), record, error) || (line && !log_line (self, &job->id, line, error)))
    return FALSE;
  publish_later (r->publications, BRO_ENGINE_EVENT_TYPE_JOB_FINISHED, record);
  return TRUE;
}

static void
add_result (GPtrArray *results, BroStepKind kind, BroStepState state, const BroBroError *error)
{
  BroStepResult *result = bro_step_result_new (kind, state);
  result->error = bro_bro_error_copy (error);
  g_ptr_array_add (results, result);
}

static gboolean
done_has (GPtrArray *done, BroStepKind kind)
{
  for (guint i = 0; i < done->len; i++)
    if (((BroStepRecord *) done->pdata[i])->kind == kind)
      return TRUE;
  return FALSE;
}

/* Requeues, resumes or finishes the job. */
static gboolean
recover_job (BroRecoveryService *self, Run *r, const BroJobRecord *job, BroBroError **error)
{
  static const BroOutcomePolicy policy = { 0 };
  g_autoptr (GPtrArray) rows = bro_step_repository_load (bro_store_steps (self->store), job->id, error);
  g_autoptr (GPtrArray) done = NULL;
  g_autoptr (GPtrArray) results = NULL;
  g_autoptr (GArray) initial = NULL;
  GArray *steps;
  BroStepRecord *row = NULL;
  BroStepKind kind;
  BroStep *step;
  BroResumability resumability;
  const char *wanted;
  gboolean resumes;
  gboolean started;
  const char *step_name;
  if (!rows)
    return FALSE;
  done = bro_job_planner_done (rows);
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_result_free);
  for (guint i = 0; i < done->len; i++)
    {
      BroStepRecord *s = done->pdata[i];
      add_result (results, s->kind, s->state, s->error);
      ((BroStepResult *) results->pdata[results->len - 1])->read_errors = s->output ? s->output->read_errors : 0;
    }
  if (job->plan)
    steps = job->plan->steps;
  else
    steps = initial = bro_job_planner_initial (job->request);
  for (guint i = 0; i < rows->len; i++)
    if (((BroStepRecord *) rows->pdata[i])->seq == (int) done->len)
      row = rows->pdata[i];

  /* The runner stopped between a step that ended (or the last one) and the job's record. */
  if (done->len >= steps->len || (row && (row->state == BRO_STEP_STATE_FAILED || row->state == BRO_STEP_STATE_CANCELLED)))
    {
      gboolean cancelled = row && row->state == BRO_STEP_STATE_CANCELLED;
      if (row && done->len < steps->len)
        add_result (results, row->kind, row->state, row->error);
      return finish (self, r, job, bro_job_outcome_decide (results, cancelled, &policy), NULL, error);
    }

  kind = g_array_index (steps, BroStepKind, done->len);
  step_name = bro_step_kind_to_wire (kind);
  started = row && (row->state == BRO_STEP_STATE_RUNNING || row->state == BRO_STEP_STATE_INTERRUPTED);
  step = bro_step_registry_step (self->registry, kind);
  resumability = step ? bro_step_resumability (step) : BRO_RESUMABILITY_NOT_RESUMABLE;
  wanted = step ? bro_step_resume_checkpoint (step) : NULL;
  resumes = resumability == BRO_RESUMABILITY_RESUME_FROM && wanted && row && row->checkpoint
            && g_strcmp0 (bro_json_value_get_string (bro_json_value_member (row->checkpoint, "kind"), NULL), wanted) == 0;
  if (started && (resumability == BRO_RESUMABILITY_NOT_RESUMABLE || (resumability == BRO_RESUMABILITY_RESUME_FROM && !resumes)))
    {
      if (!save_step (self, row, BRO_STEP_STATE_INTERRUPTED, error))
        return FALSE;
      add_result (results, kind, BRO_STEP_STATE_INTERRUPTED, NULL);
      return finish (self, r, job, bro_job_outcome_decide (results, FALSE, &policy),
                     message (BRO_MSG_RECOVERY_INTERRUPTED, BRO_SEVERITY_ERROR, "step", step_name), error);
    }
  if (started && resumability == BRO_RESUMABILITY_RESUME_FROM)
    return requeue (self, r, job, job->queue, message (BRO_MSG_RECOVERY_RESUMED, BRO_SEVERITY_INFO, "step", step_name), error);
  /* Past Acquire, or keeping an interrupted job's files apart (which needs no drive): resumed. */
  if (done_has (done, BRO_STEP_KIND_ACQUIRE) || kind == BRO_STEP_KIND_KEEP_APART)
    return requeue (self, r, job,
                    done_has (done, BRO_STEP_KIND_RELEASE) || kind == BRO_STEP_KIND_KEEP_APART ? BRO_QUEUE_PROCESSING : job->queue,
                    message (BRO_MSG_RECOVERY_RESUMED, BRO_SEVERITY_INFO, "step", step_name), error);

  /* Before Acquire. A media generation doesn't outlive the engine (owner, 2026-10-14): an automatic job can't know its
   * disc is still there (the new run's insert starts a fresh job), and a manual one reads the drive again. */
  if (job->request->drive_id && job->request->automatic)
    {
      BroDecided *decided = g_new0 (BroDecided, 1);
      BroJsonValue *params = bro_json_value_new_object ();
      g_autoptr (BroBroMessage) why = bro_bro_message_new (BRO_MSG_DISC_CHANGED_GENERATION, NULL, BRO_SEVERITY_INFO);
      if (started && !save_step (self, row, BRO_STEP_STATE_INTERRUPTED, error))
        {
          g_free (decided);
          bro_json_value_unref (params);
          return FALSE;
        }
      bro_json_value_set (params, "why", bro_bro_message_to_json (why));
      decided->outcome = BRO_OUTCOME_CANCELLED;
      decided->error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_DISC_CHANGED), bro_json_value_ref (params), NULL);
      return finish (self, r, job, decided, bro_bro_message_new (BRO_MSG_DISC_CHANGED, params, BRO_SEVERITY_ERROR), error);
    }
  if (job->request->drive_id)
    {
      g_autoptr (BroJobRecord) again = bro_job_record_copy (job);
      for (guint i = 0; i < rows->len; i++)
        {
          g_autoptr (BroStepRecord) reset = bro_step_record_copy (rows->pdata[i]);
          reset->state = BRO_STEP_STATE_PENDING;
          reset->has_started_at = FALSE;
          reset->has_finished_at = FALSE;
          g_clear_pointer (&reset->output, bro_step_output_free);
          g_clear_pointer (&reset->error, bro_bro_error_free);
          g_clear_pointer (&reset->checkpoint, bro_json_value_unref);
          if (!bro_step_repository_save (bro_store_steps (self->store), reset, error))
            return FALSE;
        }
      g_clear_pointer (&again->plan, bro_job_plan_free);
      again->has_media_generation = FALSE;
      again->request->has_media_generation = FALSE;
      return requeue (self, r, again, again->queue, message (BRO_MSG_RECOVERY_REQUEUED, BRO_SEVERITY_INFO, NULL, NULL), error);
    }
  return requeue (self, r, job, job->queue, message (BRO_MSG_RECOVERY_REQUEUED, BRO_SEVERITY_INFO, NULL, NULL), error);
}

static gboolean
jobs_work (gpointer data, BroBroError **error)
{
  Run *r = data;
  BroRecoveryService *self = r->self;
  g_autoptr (GPtrArray) jobs = bro_job_repository_active (bro_store_jobs (self->store), error);
  g_autoptr (GPtrArray) homes = NULL;
  g_autoptr (GPtrArray) found = NULL;
  if (!jobs)
    return FALSE;
  r->count = jobs->len;
  homes = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < jobs->len; i++)
    {
      g_autofree char *directory = job_directory (self, &((BroJobRecord *) jobs->pdata[i])->id);
      g_ptr_array_add (homes, g_strconcat (directory, "/home", NULL));
    }
  found = bro_settings_isolation_recover (self->isolation, homes);
  for (guint i = 0; i < found->len; i++)
    {
      BroBroMessage *problem = found->pdata[i];
      const char *path = bro_json_value_get_string (bro_json_value_member (problem->params, "path"), "");
      BroJobRecord *owner = NULL;
      for (guint k = 0; k < jobs->len && !owner; k++)
        {
          g_autofree char *directory = job_directory (self, &((BroJobRecord *) jobs->pdata[k])->id);
          g_autofree char *prefix = g_strconcat (directory, "/", NULL);
          if (g_str_has_prefix (path, prefix))
            owner = jobs->pdata[k];
        }
      if (owner)
        {
          if (!log_line (self, &owner->id, problem, error))
            return FALSE;
        }
      else
        {
          Publication *p = g_new0 (Publication, 1);
          p->problem = bro_bro_message_copy (problem);
          g_ptr_array_add (r->publications, p);
        }
    }
  for (guint i = 0; i < jobs->len; i++)
    if (!recover_job (self, r, jobs->pdata[i], error))
      return FALSE;
  return TRUE;
}

/* ---- libraries and files (workers) ---- */

/* Removes what a stopped write leaves (bro_archive_files_is_leftover) from the folder, and its subfolders when
 * @recursive. */
static void
remove_leftovers (BroFileSystem *fs, const char *folder, gboolean recursive)
{
  g_autoptr (GPtrArray) entries = NULL;
  if (!bro_file_system_exists (fs, folder) || !(entries = bro_file_system_list (fs, folder, NULL)))
    return;
  for (guint i = 0; i < entries->len; i++)
    {
      BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *path = g_strconcat (folder, "/", e->name, NULL);
      if (e->is_directory)
        {
          if (recursive)
            remove_leftovers (fs, path, TRUE);
        }
      else if (bro_archive_files_is_leftover (e->name))
        bro_file_system_remove (fs, path, NULL);
    }
}

typedef struct {
  Run *run;
  char *library_id; /* NULL: the data folder */
  char *root;
  GPtrArray *publications; /* Publication: the jobs a roll forward gave a new outcome */
} Tidy;

static void
tidy_free (gpointer data)
{
  Tidy *t = data;
  run_unref (t->run);
  g_ptr_array_unref (t->publications);
  g_free (t->library_id);
  g_free (t->root);
  g_free (t);
}

/* A job that finished failed in its Commit or KeepApart step, or cancelled in its Commit step or before a retried Commit
 * ran, whose commit a roll forward has just finished, gets the outcome that step gives when it succeeds (owner I1-3,
 * 2026-11-03 and 2026-11-04, F9's rule): bro_job_outcome_decide of its steps with that step succeeded, so succeeded or
 * succeededWithReadErrors after a Commit, cancelled after a KeepApart. That step is the first row not done, failed or
 * cancelled: a cancel stores it cancelled, or failed when it lands just after the step ended; or pending in a cancelled job:
 * a retry stored it, and the cancel came before the step ran again. Any other job with a pending row keeps its outcome. Its
 * finished_at, step rows and log stay as they were; its jobFinished goes into @publications. */
static gboolean
committed (BroRecoveryService *self, const BroId *job_id, GPtrArray *publications, BroBroError **error)
{
  static const BroOutcomePolicy policy = { 0 };
  g_autoptr (BroJobRecord) job = bro_job_repository_load (bro_store_jobs (self->store), *job_id, error);
  g_autoptr (GPtrArray) rows = NULL;
  g_autoptr (GPtrArray) done = NULL;
  g_autoptr (GPtrArray) results = NULL;
  g_autoptr (BroDecided) decided = NULL;
  BroStepRecord *next = NULL;
  if (!job)
    return !error || !*error; /* none: nothing to do */
  if (job->state != BRO_JOB_STATE_FINISHED || !job->has_outcome || (job->outcome != BRO_OUTCOME_FAILED && job->outcome != BRO_OUTCOME_CANCELLED))
    return TRUE;
  if (!(rows = bro_step_repository_load (bro_store_steps (self->store), *job_id, error)))
    return FALSE;
  done = bro_job_planner_done (rows);
  for (guint i = 0; i < rows->len; i++)
    if (((BroStepRecord *) rows->pdata[i])->seq == (int) done->len)
      next = rows->pdata[i];
  if (!next || (next->state != BRO_STEP_STATE_FAILED && next->state != BRO_STEP_STATE_CANCELLED && next->state != BRO_STEP_STATE_PENDING))
    return TRUE;
  if (next->state == BRO_STEP_STATE_PENDING && job->outcome != BRO_OUTCOME_CANCELLED)
    return TRUE;
  if (next->kind != BRO_STEP_KIND_COMMIT && !(next->kind == BRO_STEP_KIND_KEEP_APART && job->outcome == BRO_OUTCOME_FAILED))
    return TRUE;
  results = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_result_free);
  for (guint i = 0; i < done->len; i++)
    {
      BroStepRecord *s = done->pdata[i];
      add_result (results, s->kind, s->state, s->error);
      ((BroStepResult *) results->pdata[results->len - 1])->read_errors = s->output ? s->output->read_errors : 0;
    }
  add_result (results, next->kind, BRO_STEP_STATE_SUCCEEDED, NULL);
  decided = bro_job_outcome_decide (results, FALSE, &policy);
  job->outcome = decided->outcome;
  bro_bro_error_free (job->error);
  job->error = bro_bro_error_copy (decided->error);
  if (!bro_job_repository_update (bro_store_jobs (self->store), job, error))
    return FALSE;
  publish_later (publications, BRO_ENGINE_EVENT_TYPE_JOB_FINISHED, job);
  return TRUE;
}

/* A failure is logged on its job (or left for the next start); it never stops the others. */
static void
roll_forward_all (BroRecoveryService *self, const char *library_id, GPtrArray *publications)
{
  g_autoptr (GPtrArray) intents = bro_unit_repository_open_intents (bro_store_units (self->store), NULL);
  for (guint i = 0; intents && i < intents->len; i++)
    {
      BroCommitIntent *intent = intents->pdata[i];
      g_autoptr (BroUnitRecord) unit = bro_unit_repository_get (bro_store_units (self->store), intent->unit_id, NULL);
      g_autoptr (BroBroError) e = NULL;
      g_autoptr (BroCommitResult) result = NULL;
      if (!unit || strcmp (unit->library_id, library_id) != 0)
        continue;
      result = bro_library_service_roll_forward (self->library, intent, &e);
      if (result)
        {
          g_autoptr (BroBroMessage) line = message (BRO_MSG_COMMIT_ROLLED_FORWARD, BRO_SEVERITY_WARNING, "unit", intent->unit_id.value);
          log_line (self, &intent->job_id, line, NULL);
          if (committed (self, &intent->job_id, publications, &e))
            remove_leftovers (self->fs, intent->destination, FALSE);
        }
      if (e) /* the roll forward or the outcome failed (neither: the job's Commit finished it meanwhile) */
        {
          /* A roll forward that failed keeps its intent: the job's Commit step rolls it forward when it runs again. A job whose
           * new outcome couldn't be stored keeps its old one with this line in its log; a retry from Commit finds the unit
           * committed. */
          BroMessageCode code = BRO_MSG_INTERNAL_UNEXPECTED;
          g_autoptr (BroBroMessage) line = NULL;
          bro_message_code_parse (e->code, &code);
          line = bro_bro_message_new (code, bro_json_value_ref (e->params), BRO_SEVERITY_WARNING);
          log_line (self, &intent->job_id, line, NULL);
        }
    }
}

/* A BroSealWriteFunc that keeps each log copy in @data (GHashTable: name → GBytes). */
static gboolean
keep_copy (gpointer data, const char *name, GBytes *bytes, BroBroError **error)
{
  g_hash_table_replace (data, g_strdup (name), g_bytes_ref (bytes));
  return TRUE;
}

/* The name a failed job's staging is set aside under: KeepApart's (bro_keep_apart_step_folder_name) for the job's request,
 * plan and succeeded step outputs in seq order (owner H3a-2). Rows that can't be loaded leave the outputs out (the name
 * from the job alone, as the Swift pass does). Transfer full. */
static char *
set_aside_name (BroRecoveryService *self, const BroJobRecord *job)
{
  g_autoptr (GPtrArray) rows = bro_step_repository_load (bro_store_steps (self->store), job->id, NULL);
  g_autoptr (BroJobSnapshot) snap = bro_job_snapshot_new (&job->id, bro_job_request_copy (job->request), bro_job_plan_copy (job->plan));
  for (guint i = 0; rows && i < rows->len; i++)
    {
      const BroStepRecord *row = rows->pdata[i];
      if (row->state == BRO_STEP_STATE_SUCCEEDED && row->output)
        g_ptr_array_add (snap->outputs, bro_step_output_copy (row->output));
    }
  return bro_keep_apart_step_folder_name (snap);
}

/* The job's logs as Seal copies them (_bro_seal_copy_logs_from: its log as text, MakeMKV's transcripts, each when the job
 * folder has it; none when its Plan's profile has archive.archiveRecord false) into @logs (name → GBytes), their names
 * into @names. When they can't be read, there are none: the files are set aside without them. */
static void
copy_logs (BroRecoveryService *self, const BroJobRecord *job, GHashTable *logs, GPtrArray *names)
{
  g_autoptr (GPtrArray) rows = bro_step_repository_load (bro_store_steps (self->store), job->id, NULL);
  g_autoptr (BroPlanOutput) plan = NULL;
  g_autoptr (GPtrArray) log = NULL;
  g_autofree char *directory = job_directory (self, &job->id);
  BroId unit = bro_archive_files_unit_id (&job->id);
  char unit8[9];
  if (!rows)
    return;
  for (guint i = rows->len; i > 0 && !plan; i--)
    {
      const BroStepRecord *row = rows->pdata[i - 1];
      if (row->kind == BRO_STEP_KIND_PLAN && row->state == BRO_STEP_STATE_SUCCEEDED && row->output && row->output->data
          && row->output->data->kind == BRO_JSON_VALUE_OBJECT)
        plan = bro_plan_output_decode (row->output->data);
    }
  g_strlcpy (unit8, unit.value, sizeof unit8);
  log = _bro_seal_copy_logs_from (self->fs, directory, self->renderer, unit8,
                                  bro_json_value_get_bool (bro_json_value_member (bro_json_value_member (plan ? plan->profile : NULL, "archive"),
                                                                                  "archiveRecord"),
                                                           TRUE),
                                  keep_copy, logs, names, NULL);
  if (!log)
    {
      g_hash_table_remove_all (logs);
      g_ptr_array_set_size (names, 0);
    }
}

static gboolean
tidy_work (gpointer data, BroBroError **error)
{
  Tidy *t = data;
  Run *r = t->run;
  BroRecoveryService *self = r->self;
  g_autoptr (GPtrArray) open = NULL;
  g_autoptr (GHashTable) committing = NULL;
  g_autoptr (GPtrArray) folders = NULL;
  if (!t->library_id)
    {
      remove_leftovers (self->fs, self->data, TRUE);
      return TRUE;
    }
  roll_forward_all (self, t->library_id, t->publications);
  committing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  open = bro_unit_repository_open_intents (bro_store_units (self->store), NULL);
  for (guint i = 0; open && i < open->len; i++)
    g_hash_table_add (committing, g_strdup (((BroCommitIntent *) open->pdata[i])->staging));
  folders = bro_library_service_staging_folders (self->library, t->library_id, NULL);
  for (guint i = 0; folders && i < folders->len; i++)
    {
      const char *folder = folders->pdata[i];
      g_autofree char *path = g_strconcat (t->root, "/.bromelia/staging/", folder, NULL);
      BroId id = { { 0 } };
      g_autoptr (BroJobRecord) job = NULL;
      g_autoptr (GPtrArray) lines = NULL;
      g_autoptr (GPtrArray) read_errors = NULL;
      g_autoptr (GBytes) note = NULL;
      g_autofree char *title = NULL;
      g_autofree char *dest = NULL;
      g_autofree char *full_log = NULL;
      g_autoptr (GHashTable) logs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      GString *text;
      g_strlcpy (id.value, folder, sizeof id.value);
      remove_leftovers (self->fs, path, TRUE);
      if (g_hash_table_contains (r->unfinished, folder) || g_hash_table_contains (committing, path))
        continue;
      job = bro_job_repository_load (bro_store_jobs (self->store), id, NULL);
      if (job && job->has_outcome && job->outcome == BRO_OUTCOME_INTERRUPTED)
        continue; /* kept so it can be continued (owner, 2026-10-03) */
      if (job)
        copy_logs (self, job, logs, names);
      g_ptr_array_add (names, NULL);
      read_errors = g_ptr_array_new ();
      if (job)
        {
          g_autofree char *directory = job_directory (self, &job->id);
          full_log = g_strconcat (directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
        }
      lines = bro_notes_render (&id, job && job->has_outcome ? job->outcome : BRO_OUTCOME_INTERRUPTED, job ? job->error : NULL, read_errors,
                                (const char *const *) names->pdata, full_log);
      text = g_string_new ("");
      for (guint k = 0; k < lines->len; k++)
        {
          g_autofree char *rendered = bro_message_renderer_render (self->renderer, lines->pdata[k]);
          g_string_append (text, rendered);
          g_string_append_c (text, '\n');
        }
      note = g_string_free_to_bytes (text);
      title = job ? set_aside_name (self, job) : g_strdup (folder);
      /* a failure leaves it hidden in staging; the next start tries again */
      dest = bro_library_service_set_aside (self->library, t->library_id, folder, title, note, logs, NULL);
    }
  return TRUE;
}

static void
complete (Run *r)
{
  BroBroMessage *summary = NULL;
  if (r->finished)
    return;
  r->finished = TRUE;
  if (r->timer)
    bro_engine_timer_cancel (r->timer);
  g_clear_object (&r->timer);
  if (r->count > 0)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "count", bro_json_value_new_integer (r->count));
      summary = bro_bro_message_new (BRO_MSG_RECOVERY_SUMMARY, params, BRO_SEVERITY_WARNING);
    }
  r->done (summary, NULL, r->done_data);
}

static void
tidy_done (gpointer data, const BroBroError *error)
{
  Tidy *t = data;
  for (guint i = 0; i < t->publications->len; i++)
    {
      Publication *p = t->publications->pdata[i];
      bro_engine_event_unref (bro_event_bus_publish (t->run->self->bus, p->type, p->data));
    }
  if (--t->run->pending == 0)
    complete (t->run);
}

static void
deadline (gpointer data)
{
  complete (data);
}

static void
start_tidy (Run *r, const char *library_id, const char *root)
{
  Tidy *t = g_new0 (Tidy, 1);
  t->run = run_ref (r);
  t->library_id = g_strdup (library_id);
  t->root = g_strdup (root);
  t->publications = g_ptr_array_new_with_free_func (publication_free);
  r->pending++;
  bro_engine_executor_worker (r->self->executor, tidy_work, tidy_done, t, tidy_free);
}

static void
jobs_done (gpointer data, const BroBroError *error)
{
  Run *r = data;
  BroRecoveryService *self = r->self;
  g_autoptr (BroConfig) config = NULL;
  g_autoptr (GPtrArray) libraries = NULL;
  g_autoptr (BroTimerSchedule) at = bro_timer_schedule_new ();
  if (error)
    {
      r->finished = TRUE;
      r->done (NULL, error, r->done_data);
      return;
    }
  for (guint i = 0; i < r->publications->len; i++)
    {
      Publication *p = r->publications->pdata[i];
      if (p->problem)
        bro_problem_dto_free (bro_problem_service_raise (self->problems, p->problem, NULL, NULL, 0));
      else
        bro_engine_event_unref (bro_event_bus_publish (self->bus, p->type, p->data));
    }
  config = bro_config_source_current (self->config);
  libraries = bro_config_libraries (config);
  at->kind = BRO_TIMER_SCHEDULE_AT;
  at->instant.unix_milliseconds = bro_engine_clock_now (self->clock).unix_milliseconds + PATIENCE_MS;
  /* Not a reference: the run outlives the timer (complete cancels it before the last worker lets go), and a clock
   * that keeps its timers would otherwise hold the run through the service. */
  r->timer = bro_engine_clock_timer (self->clock, at, deadline, r, NULL);
  for (guint i = 0; i < libraries->len; i++)
    {
      BroLibraryEntry *l = libraries->pdata[i];
      start_tidy (r, l->id, l->path);
    }
  start_tidy (r, NULL, NULL);
}

void
bro_recovery_service_recover (BroRecoveryService *self, BroRecoveryDone done, gpointer data, GDestroyNotify destroy)
{
  Run *r = g_new0 (Run, 1);
  g_atomic_ref_count_init (&r->refs);
  r->self = g_object_ref (self);
  r->done = done;
  r->done_data = data;
  r->done_destroy = destroy;
  r->publications = g_ptr_array_new_with_free_func (publication_free);
  r->unfinished = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  bro_engine_executor_worker (self->executor, jobs_work, jobs_done, r, run_unref);
}
