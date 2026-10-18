/* bro-job-service.c */
#include "bro-job-service.h"

#include "bro-job-log.h"
#include "bro-job-outcome.h"
#include "bro-job-planner.h"
#include "bro-log-line-dto.h"
#include "bro-message-code.h"
#include <string.h>

struct _BroJobService {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroStore *store;
  BroScheduler *scheduler;
  BroPipelineRunner *runner;
  BroEngineClock *clock;
  BroIdGenerator *ids;
  BroFileSystem *fs;
  char *data;
  GHashTable *active;    /* job id → Active: the jobs this service runs, until their run ends */
  GHashTable *asked;     /* job id → GINT_TO_POINTER (position + 1): the positions last asked of the store */
  GMutex lock;           /* guards positions: the store's workers read it */
  GHashTable *positions; /* job id → GINT_TO_POINTER (position + 1): where each active job should be stored */
  BroSubscription *queues;
};

G_DEFINE_FINAL_TYPE (BroJobService, bro_job_service, G_TYPE_OBJECT)

typedef struct {
  gboolean cancel_requested;
} Active;

typedef enum { OP_NONE, OP_MOVE, OP_START_NOW } Op;

/* One command (or an internal job of the service): what its workers need and whom it answers. */
typedef struct {
  BroJobService *self; /* a reference */
  BroId job_id;
  BroJobRecord *record; /* loaded, or to write */
  GPtrArray *records;   /* BroJobRecord: start, query */
  BroJobQuery *query;
  int total;
  int position;
  gboolean forget;      /* finishing a job nothing runs: also give it up */
  gboolean must_load;   /* log: the job must exist */
  char *log_path;
  GPtrArray *lines;     /* BroLogLineDto */
  gint64 after;
  char *decision_id;
  char *answer;
  Op op;
  int by;
  BroJobServiceDone done;
  BroJobDtoDone job_done;
  BroJobPageDone page_done;
  BroLogLinesDone lines_done;
  gpointer data;
  GDestroyNotify destroy;
} Call;

static Call *
call_new (BroJobService *self, gpointer data, GDestroyNotify destroy)
{
  Call *c = g_new0 (Call, 1);
  c->self = g_object_ref (self);
  c->data = data;
  c->destroy = destroy;
  return c;
}

static void
call_free (gpointer data)
{
  Call *c = data;
  if (c->destroy)
    c->destroy (c->data);
  g_object_unref (c->self);
  bro_job_record_free (c->record);
  g_clear_pointer (&c->records, g_ptr_array_unref);
  bro_job_query_free (c->query);
  g_free (c->log_path);
  g_clear_pointer (&c->lines, g_ptr_array_unref);
  g_free (c->decision_id);
  g_free (c->answer);
  g_free (c);
}

/* A new call that answers whoever @from answers; @from no longer does. */
static Call *
call_hand_off (Call *from)
{
  Call *to = call_new (from->self, from->data, from->destroy);
  to->done = from->done;
  to->job_done = from->job_done;
  to->page_done = from->page_done;
  to->lines_done = from->lines_done;
  from->done = NULL;
  from->job_done = NULL;
  from->page_done = NULL;
  from->lines_done = NULL;
  from->data = NULL;
  from->destroy = NULL;
  return to;
}

/* Answers the call with @error (any kind of answer). */
static void
fail (Call *c, const BroBroError *error)
{
  if (c->done)
    c->done (error, c->data);
  if (c->job_done)
    c->job_done (NULL, error, c->data);
  if (c->page_done)
    c->page_done (NULL, error, c->data);
  if (c->lines_done)
    c->lines_done (NULL, error, c->data);
  c->done = NULL;
  c->job_done = NULL;
  c->page_done = NULL;
  c->lines_done = NULL;
}

static void
fail_with (Call *c, BroMessageCode code, const BroId *job_id)
{
  BroJsonValue *params = bro_json_value_new_object ();
  g_autoptr (BroBroError) error = NULL;
  bro_json_value_set (params, "jobId", bro_json_value_new_string (job_id->value));
  error = bro_bro_error_new (bro_message_code_wire (code), params, NULL);
  fail (c, error);
}

static void
bad_request (Call *c, const char *detail)
{
  BroJsonValue *params = bro_json_value_new_object ();
  g_autoptr (BroBroError) error = NULL;
  bro_json_value_set (params, "detail", bro_json_value_new_string (detail));
  error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_SERVER_BAD_REQUEST), params, NULL);
  fail (c, error);
}

static void
succeed (Call *c)
{
  if (c->done)
    c->done (NULL, c->data);
  c->done = NULL;
}

/* ---- positions ---- */

static int
get_int (GHashTable *table, const BroId *id)
{
  gpointer v = g_hash_table_lookup (table, id->value);
  return v ? GPOINTER_TO_INT (v) - 1 : -1;
}

static void
set_position (BroJobService *self, const BroId *id, int position)
{
  g_mutex_lock (&self->lock);
  g_hash_table_replace (self->positions, g_strdup (id->value), GINT_TO_POINTER (position + 1));
  g_mutex_unlock (&self->lock);
}

static gboolean
position_block (BroStore *store, gpointer data, BroBroError **error)
{
  Call *c = data;
  BroJobService *self = c->self;
  gpointer v;
  int position;
  g_autoptr (BroJobRecord) record = NULL;
  g_mutex_lock (&self->lock);
  v = g_hash_table_lookup (self->positions, c->job_id.value);
  g_mutex_unlock (&self->lock);
  if (!v)
    return TRUE;
  position = GPOINTER_TO_INT (v) - 1;
  record = bro_job_repository_load (bro_store_jobs (store), c->job_id, error);
  if (!record)
    return *error == NULL;
  if (record->state == BRO_JOB_STATE_FINISHED || record->position == position)
    return TRUE;
  record->position = position;
  return bro_job_repository_update (bro_store_jobs (store), record, error);
}

static gboolean
position_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  g_autoptr (BroBroError) ignored = NULL; /* best effort: the order only matters after a restart */
  bro_store_transaction (c->self->store, position_block, c, &ignored);
  return TRUE;
}

static void
nothing_done (gpointer data, const BroBroError *error)
{
}

/* Writes the job's place as it is when the write runs (so a later move wins over an earlier write that lands last), in
 * a transaction that changes nothing else. */
static void
save_position (BroJobService *self, const BroId *job_id)
{
  Call *c = call_new (self, NULL, NULL);
  c->job_id = *job_id;
  bro_engine_executor_worker (self->executor, position_work, nothing_done, c, call_free);
}

/* Stores the place of every active job whose place in its queue changed. */
static void
on_queue_changed (BroEngineEvent *event, gpointer data)
{
  BroJobService *self = data;
  g_autoptr (BroQueueOrders) orders = bro_scheduler_queues (self->scheduler);
  GArray *lists[] = { orders->acquisition, orders->processing, orders->maintenance };
  for (guint q = 0; q < G_N_ELEMENTS (lists); q++)
    for (guint i = 0; i < lists[q]->len; i++)
      {
        BroId *id = &g_array_index (lists[q], BroId, i);
        int asked = get_int (self->asked, id);
        if (asked < 0 || asked == (int) i) /* not admitted yet, or in place */
          continue;
        g_hash_table_replace (self->asked, g_strdup (id->value), GINT_TO_POINTER (i + 1));
        set_position (self, id, i);
        save_position (self, id);
      }
}

/* ---- running ---- */

/* Puts the job in its queue (in call order) and in the active view; its place. */
static int
admit (BroJobService *self, const BroId *job_id, BroQueue queue)
{
  int position;
  g_hash_table_replace (self->active, g_strdup (job_id->value), g_new0 (Active, 1));
  bro_scheduler_submit (self->scheduler, job_id, queue);
  position = MAX (bro_scheduler_position (self->scheduler, job_id), 0);
  g_hash_table_replace (self->asked, g_strdup (job_id->value), GINT_TO_POINTER (position + 1));
  set_position (self, job_id, position);
  return position;
}

static void
forget (BroJobService *self, const BroId *job_id)
{
  g_hash_table_remove (self->active, job_id->value);
  g_hash_table_remove (self->asked, job_id->value);
  g_mutex_lock (&self->lock);
  g_hash_table_remove (self->positions, job_id->value);
  g_mutex_unlock (&self->lock);
}

static gboolean
update_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  return bro_job_repository_update (bro_store_jobs (c->self->store), c->record, error);
}

static void
cancelled_done (gpointer data, const BroBroError *error)
{
  Call *c = data;
  BroJobService *self = c->self;
  if (!error)
    {
      g_autoptr (BroJobDto) dto = bro_job_dto_of (c->record);
      g_autoptr (BroJsonValue) event = bro_json_value_new_object ();
      bro_json_value_set (event, "job", bro_job_dto_json (dto));
      bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_JOB_FINISHED, event));
    }
  if (c->forget)
    {
      bro_scheduler_finish (self->scheduler, &c->record->id);
      forget (self, &c->record->id);
    }
  if (error)
    fail (c, error);
  else
    succeed (c);
}

/* Finishes a job nothing runs as cancelled (job.cancelledByUser), then answers @c (taken). */
static void
finish_cancelled (Call *c, const BroJobRecord *record, gboolean also_forget)
{
  g_autoptr (GPtrArray) none = g_ptr_array_new ();
  g_autoptr (BroDecided) decided = bro_job_outcome_decide (none, TRUE, NULL);
  BroJobRecord *finished = bro_job_record_copy (record); /* @record may be c->record */
  bro_job_record_free (c->record);
  c->record = finished;
  c->record->state = BRO_JOB_STATE_FINISHED;
  c->record->has_outcome = TRUE;
  c->record->outcome = decided->outcome;
  bro_bro_error_free (c->record->error);
  c->record->error = bro_bro_error_copy (decided->error);
  g_clear_pointer (&c->record->decision, bro_json_value_unref);
  g_clear_pointer (&c->record->blocked_by, bro_json_value_unref);
  c->record->has_finished_at = TRUE;
  c->record->finished_at = bro_engine_clock_now (c->self->clock);
  c->forget = also_forget;
  bro_engine_executor_worker (c->self->executor, update_work, cancelled_done, c, call_free);
}

/* A run's done data. Weak: the runner keeps a job that never ends (a question nobody answers) until it goes, and a
 * strong reference would keep the service, and so the runner, alive for ever. */
typedef struct {
  GWeakRef self;
  BroId id;
} Track;

static void
track_free (gpointer data)
{
  Track *t = data;
  g_weak_ref_clear (&t->self);
  g_free (t);
}

static void
run_done (const BroDecided *decided, const BroBroError *error, gpointer data)
{
  Track *t = data;
  g_autoptr (BroJobService) self = g_weak_ref_get (&t->self);
  if (!self)
    return;
  if (error)
    bro_scheduler_finish (self->scheduler, &t->id); /* its record was gone: nothing ran */
  forget (self, &t->id);
}

/* Starts the job's run (live in the runner at once: a cancel from now on reaches it), or finishes it as cancelled when a
 * cancel came first. */
static void
launch (BroJobService *self, const BroJobRecord *record)
{
  Active *a = g_hash_table_lookup (self->active, record->id.value);
  Track *t;
  if (a->cancel_requested)
    {
      finish_cancelled (call_new (self, NULL, NULL), record, TRUE);
      return;
    }
  t = g_new0 (Track, 1);
  g_weak_ref_init (&t->self, self);
  t->id = record->id;
  bro_pipeline_runner_run (self->runner, &record->id, run_done, t, track_free);
}

/* ---- loading ---- */

static gboolean
load_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  c->record = bro_job_repository_load (bro_store_jobs (c->self->store), c->job_id, error);
  return c->record != NULL || *error == NULL;
}

/* Loads the job, then @then (the call taken; job.notFound answered here). */
typedef void (*Then) (Call *c);

typedef struct {
  Call *call;
  Then then;
} Loading;

static gboolean
loading_work (gpointer data, BroBroError **error)
{
  return load_work (((Loading *) data)->call, error);
}

static void
loading_done (gpointer data, const BroBroError *error)
{
  Loading *l = data;
  Call *c = l->call;
  if (error)
    fail (c, error);
  else if (!c->record)
    fail_with (c, BRO_MSG_JOB_NOT_FOUND, &c->job_id);
  else
    {
      l->call = NULL;
      l->then (c);
    }
}

static void
loading_free (gpointer data)
{
  Loading *l = data;
  if (l->call)
    call_free (l->call);
  g_free (l);
}

static void
load_then (Call *c, Then then)
{
  Loading *l = g_new0 (Loading, 1);
  l->call = c;
  l->then = then;
  bro_engine_executor_worker (c->self->executor, loading_work, loading_done, l, loading_free);
}

/* ---- commands ---- */

static gboolean
active_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  c->records = bro_job_repository_active (bro_store_jobs (c->self->store), error);
  return c->records != NULL;
}

static void
start_done (gpointer data, const BroBroError *error)
{
  Call *c = data;
  BroJobService *self = c->self;
  if (error)
    {
      fail (c, error);
      return;
    }
  if (!self->queues)
    {
      BroEngineEventType types[] = { BRO_ENGINE_EVENT_TYPE_QUEUE_CHANGED };
      self->queues = bro_event_bus_subscribe (self->bus, types, 1, on_queue_changed, self, NULL);
    }
  for (guint i = 0; i < c->records->len; i++)
    {
      BroJobRecord *job = g_ptr_array_index (c->records, i);
      if (g_hash_table_contains (self->active, job->id.value))
        continue;
      admit (self, &job->id, job->queue);
      /* what the store has: on_queue_changed writes the place it has now */
      g_hash_table_replace (self->asked, g_strdup (job->id.value), GINT_TO_POINTER (job->position + 1));
      launch (self, job);
    }
  on_queue_changed (NULL, self);
  succeed (c);
}

void
bro_job_service_start (BroJobService *self, BroJobServiceDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  c->done = done;
  bro_engine_executor_worker (self->executor, active_work, start_done, c, call_free);
}

static gboolean
insert_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  return bro_job_repository_insert (bro_store_jobs (c->self->store), c->record, error);
}

static void
insert_done (gpointer data, const BroBroError *error)
{
  Call *c = data;
  BroJobService *self = c->self;
  const BroId *id = &c->record->id;
  g_autoptr (BroJobDto) dto = NULL;
  g_autoptr (BroJsonValue) event = NULL;
  int position;
  if (error)
    {
      forget (self, id);
      bro_scheduler_finish (self->scheduler, id);
      fail (c, error);
      return;
    }
  if (get_int (self->asked, id) != c->position) /* the queue moved while the record was written */
    save_position (self, id);
  dto = bro_job_dto_of (c->record);
  position = bro_scheduler_position (self->scheduler, id);
  dto->has_position = position >= 0;
  dto->position = MAX (position, 0);
  event = bro_json_value_new_object ();
  bro_json_value_set (event, "job", bro_job_dto_json (dto));
  bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_JOB_CHANGED, event));
  launch (self, c->record);
  if (c->job_done)
    c->job_done (dto, NULL, c->data);
  c->job_done = NULL;
}

void
bro_job_service_create (BroJobService *self, const BroJobRequest *request, BroJobDtoDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  BroJobRecord *r = bro_job_record_new ();
  BroQueue queue = bro_job_planner_queue (request->kind);
  g_autoptr (GArray) steps = bro_job_planner_initial (request);
  c->job_done = done;
  r->id = bro_id_generator_next (self->ids);
  c->position = admit (self, &r->id, queue);
  r->kind = request->kind;
  r->has_parent_id = request->parent_id != NULL;
  if (request->parent_id)
    r->parent_id = *request->parent_id;
  r->state = BRO_JOB_STATE_QUEUED;
  r->queue = queue;
  r->position = c->position;
  r->drive_id = g_strdup (request->drive_id);
  r->has_media_generation = request->has_media_generation;
  r->media_generation = request->media_generation;
  r->automatic = request->automatic;
  r->title = g_strdup (request->title);
  r->request = bro_job_request_copy (request);
  if (steps->len > 0)
    {
      r->plan = bro_job_plan_new ();
      g_array_append_vals (r->plan->steps, steps->data, steps->len);
    }
  r->has_unit_id = request->unit_id != NULL;
  if (request->unit_id)
    r->unit_id = *request->unit_id;
  r->created_at = bro_engine_clock_now (self->clock);
  c->record = r;
  bro_engine_executor_worker (self->executor, insert_work, insert_done, c, call_free);
}

static void
cancel_loaded (Call *c)
{
  if (c->record->state == BRO_JOB_STATE_FINISHED || g_hash_table_contains (c->self->active, c->job_id.value))
    {
      succeed (c);
      call_free (c);
      return;
    }
  finish_cancelled (c, c->record, FALSE);
}

void
bro_job_service_cancel (BroJobService *self, const BroId *job_id, BroJobServiceDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  Active *a = g_hash_table_lookup (self->active, job_id->value);
  c->done = done;
  c->job_id = *job_id;
  if (a)
    {
      a->cancel_requested = TRUE;
      bro_pipeline_runner_cancel (self->runner, job_id);
      succeed (c);
      call_free (c);
      return;
    }
  load_then (c, cancel_loaded);
}

static void
retry_loaded (Call *c)
{
  g_autoptr (BroJobRequest) request = NULL;
  Call *to;
  if (c->record->state != BRO_JOB_STATE_FINISHED || !c->record->has_outcome
      || !(c->record->outcome == BRO_OUTCOME_FAILED || c->record->outcome == BRO_OUTCOME_CANCELLED || c->record->outcome == BRO_OUTCOME_SKIPPED))
    {
      fail_with (c, BRO_MSG_JOB_NOT_RETRYABLE, &c->job_id);
      call_free (c);
      return;
    }
  request = bro_job_request_copy (c->record->request);
  request->automatic = FALSE;
  request->has_media_generation = FALSE;
  request->media_generation = 0;
  to = call_hand_off (c);
  bro_job_service_create (c->self, request, to->job_done, to->data, to->destroy);
  to->job_done = NULL;
  to->data = NULL;
  to->destroy = NULL;
  call_free (to);
  call_free (c);
}

void
bro_job_service_retry (BroJobService *self, const BroId *job_id, BroJobDtoDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  c->job_done = done;
  c->job_id = *job_id;
  load_then (c, retry_loaded);
}

static void
decide_now (Call *c)
{
  g_autoptr (BroJobDto) job = bro_pipeline_runner_job (c->self->runner, &c->job_id);
  g_autoptr (BroBroError) error = NULL;
  if (!job || !job->decision || strcmp (job->decision->id, c->decision_id) != 0)
    fail_with (c, BRO_MSG_JOB_NOT_AWAITING_DECISION, &c->job_id);
  else if (!bro_pipeline_runner_decide (c->self->runner, &c->job_id, c->answer, &error))
    fail (c, error);
  else
    succeed (c);
  call_free (c);
}

void
bro_job_service_decide (BroJobService *self, const BroId *job_id, const char *decision_id, const char *answer, BroJobServiceDone done,
                        gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  c->done = done;
  c->job_id = *job_id;
  c->decision_id = g_strdup (decision_id);
  c->answer = g_strdup (answer);
  if (g_hash_table_contains (self->active, job_id->value))
    decide_now (c);
  else
    load_then (c, decide_now);
}

/* Moves the job, or puts it first, unless it doesn't wait to start a step (queued, waiting for resources, blocked). */
static void
reorder (Call *c)
{
  BroJobService *self = c->self;
  g_autoptr (BroJobDto) job = NULL;
  g_autoptr (BroBroError) error = NULL;
  BroJobState state;
  if (!g_hash_table_contains (self->active, c->job_id.value))
    {
      fail_with (c, BRO_MSG_JOB_NOT_QUEUED, &c->job_id);
      call_free (c);
      return;
    }
  job = bro_pipeline_runner_job (self->runner, &c->job_id);
  state = job ? job->state : BRO_JOB_STATE_QUEUED;
  if ((state != BRO_JOB_STATE_QUEUED && state != BRO_JOB_STATE_WAITING_FOR_RESOURCES && state != BRO_JOB_STATE_BLOCKED)
      || bro_scheduler_position (self->scheduler, &c->job_id) < 0)
    fail_with (c, BRO_MSG_JOB_NOT_QUEUED, &c->job_id);
  else if (c->op == OP_MOVE ? !bro_scheduler_move (self->scheduler, &c->job_id, c->by, &error)
                            : !bro_scheduler_start_now (self->scheduler, &c->job_id, &error))
    fail (c, error);
  else
    succeed (c);
  call_free (c);
}

static void
reorder_command (BroJobService *self, const BroId *job_id, Op op, int by, BroJobServiceDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  c->done = done;
  c->job_id = *job_id;
  c->op = op;
  c->by = by;
  if (g_hash_table_contains (self->active, job_id->value))
    reorder (c);
  else
    load_then (c, reorder);
}

void
bro_job_service_move (BroJobService *self, const BroId *job_id, int by, BroJobServiceDone done, gpointer data, GDestroyNotify destroy)
{
  reorder_command (self, job_id, OP_MOVE, by, done, data, destroy);
}

void
bro_job_service_start_now (BroJobService *self, const BroId *job_id, BroJobServiceDone done, gpointer data, GDestroyNotify destroy)
{
  reorder_command (self, job_id, OP_START_NOW, 0, done, data, destroy);
}

static gboolean
query_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  BroJobRepository *jobs = bro_store_jobs (c->self->store);
  c->records = bro_job_repository_query (jobs, c->query, error);
  return c->records && bro_job_repository_count (jobs, c->query, &c->total, error);
}

static void
query_done (gpointer data, const BroBroError *error)
{
  Call *c = data;
  BroJobService *self = c->self;
  g_autoptr (BroJobPageDto) page = NULL;
  if (error)
    {
      fail (c, error);
      return;
    }
  page = bro_job_page_dto_new ();
  for (guint i = 0; i < c->records->len; i++)
    {
      BroJobRecord *r = g_ptr_array_index (c->records, i);
      BroJobDto *dto = bro_pipeline_runner_job (self->runner, &r->id);
      if (!dto)
        dto = bro_job_dto_of (r);
      if (dto->state == BRO_JOB_STATE_QUEUED || dto->state == BRO_JOB_STATE_WAITING_FOR_RESOURCES || dto->state == BRO_JOB_STATE_BLOCKED)
        {
          int position = bro_scheduler_position (self->scheduler, &r->id);
          dto->has_position = position >= 0;
          dto->position = MAX (position, 0);
        }
      g_ptr_array_add (page->items, dto);
    }
  page->total = c->total;
  if (c->records->len > 0 && c->query->offset + (int) c->records->len < c->total)
    page->next = g_strdup_printf ("%d", c->query->offset + (int) c->records->len);
  c->page_done (page, NULL, c->data);
  c->page_done = NULL;
}

void
bro_job_service_query (BroJobService *self, const BroJobQueryDto *query, BroJobPageDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  BroJobQuery *q;
  gint64 offset = 0;
  c->page_done = done;
  if (query->limit < 1 || query->limit > BRO_JOB_QUERY_DTO_MAX_LIMIT)
    {
      g_autofree char *detail = g_strdup_printf ("limit %d", query->limit);
      bad_request (c, detail);
      call_free (c);
      return;
    }
  if (query->cursor)
    {
      gboolean digits = query->cursor[0] != '\0' && strlen (query->cursor) <= 9;
      for (const char *p = query->cursor; digits && *p; p++)
        digits = g_ascii_isdigit (*p);
      if (!digits)
        {
          g_autofree char *detail = g_strconcat ("cursor ", query->cursor, NULL);
          bad_request (c, detail);
          call_free (c);
          return;
        }
      offset = g_ascii_strtoll (query->cursor, NULL, 10);
    }
  q = bro_job_query_new ();
  g_array_append_vals (q->states, query->states->data, query->states->len);
  g_array_append_vals (q->outcomes, query->outcomes->data, query->outcomes->len);
  q->has_kind = query->has_kind;
  q->kind = query->kind;
  q->has_queue = query->has_queue;
  q->queue = query->queue;
  q->drive_id = g_strdup (query->drive_id);
  q->has_unit_id = query->has_unit_id;
  q->unit_id = query->unit_id;
  q->limit = query->limit;
  q->offset = (int) offset;
  c->query = q;
  bro_engine_executor_worker (self->executor, query_work, query_done, c, call_free);
}

static gboolean
log_work (gpointer data, BroBroError **error)
{
  Call *c = data;
  if (c->must_load && !load_work (c, error))
    return FALSE;
  if (c->must_load && !c->record)
    return TRUE;
  if (bro_file_system_exists (c->self->fs, c->log_path))
    {
      g_autoptr (GBytes) bytes = bro_file_system_read (c->self->fs, c->log_path, error);
      if (!bytes)
        return FALSE;
      c->lines = bro_job_log_decode (bytes);
    }
  else
    c->lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_line_dto_free);
  return TRUE;
}

/* Answers with the lines after c->after. */
static void
answer_lines (Call *c)
{
  g_autoptr (GPtrArray) after = g_ptr_array_new ();
  for (guint i = 0; i < c->lines->len; i++)
    {
      BroLogLineDto *line = g_ptr_array_index (c->lines, i);
      if (line->seq > c->after)
        g_ptr_array_add (after, line);
    }
  c->lines_done (after, NULL, c->data);
  c->lines_done = NULL;
}

static void
log_done (gpointer data, const BroBroError *error)
{
  Call *c = data;
  if (error)
    fail (c, error);
  else if (c->must_load && !c->record)
    fail_with (c, BRO_MSG_JOB_NOT_FOUND, &c->job_id);
  else
    answer_lines (c);
}

void
bro_job_service_log (BroJobService *self, const BroId *job_id, gint64 after, BroLogLinesDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (self, data, destroy);
  c->lines_done = done;
  c->job_id = *job_id;
  c->after = after;
  c->lines = bro_pipeline_runner_log (self->runner, job_id);
  if (c->lines)
    {
      answer_lines (c);
      call_free (c);
      return;
    }
  c->must_load = !g_hash_table_contains (self->active, job_id->value);
  c->log_path = g_strconcat (self->data, "/jobs/", job_id->value, "/" BRO_JOB_LOG_FILE_NAME, NULL);
  bro_engine_executor_worker (self->executor, log_work, log_done, c, call_free);
}

/* ---- the object ---- */

BroJobService *
bro_job_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroStore *store, BroScheduler *scheduler, BroPipelineRunner *runner,
                     BroEngineClock *clock, BroIdGenerator *ids, BroFileSystem *fs, const char *data_directory)
{
  BroJobService *self = g_object_new (BRO_TYPE_JOB_SERVICE, NULL);
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->store = g_object_ref (store);
  self->scheduler = g_object_ref (scheduler);
  self->runner = g_object_ref (runner);
  self->clock = g_object_ref (clock);
  self->ids = g_object_ref (ids);
  self->fs = g_object_ref (fs);
  self->data = g_strdup (data_directory);
  return self;
}

static void
bro_job_service_finalize (GObject *object)
{
  BroJobService *self = BRO_JOB_SERVICE (object);
  if (self->queues)
    {
      bro_subscription_cancel (self->queues);
      g_object_unref (self->queues);
    }
  g_hash_table_unref (self->active);
  g_hash_table_unref (self->asked);
  g_hash_table_unref (self->positions);
  g_mutex_clear (&self->lock);
  g_object_unref (self->executor);
  g_object_unref (self->bus);
  g_object_unref (self->store);
  g_object_unref (self->scheduler);
  g_object_unref (self->runner);
  g_object_unref (self->clock);
  g_object_unref (self->ids);
  g_object_unref (self->fs);
  g_free (self->data);
  G_OBJECT_CLASS (bro_job_service_parent_class)->finalize (object);
}

static void
bro_job_service_class_init (BroJobServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_job_service_finalize;
}

static void
bro_job_service_init (BroJobService *self)
{
  self->active = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->asked = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->positions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_mutex_init (&self->lock);
}
