/* bro-local-engine.c */
#include "bro-local-engine.h"
#include "bro-api-private.h"
#include "bro-config.h"
#include "bro-message-code.h"
#include "bro-scheduler-limits.h"
#include "bro-snapshot-dto.h"

#include <string.h>

/* How long stop waits for the jobs to settle and the executor to go idle; the host exits anyway after it. */
#define SETTLE_CAP_S 30
/* How often stop looks at the executor's activity. */
#define SETTLE_POLL_MS 5
/* The finished jobs a snapshot carries, newest first. */
#define FINISHED_IN_SNAPSHOT 50

struct _BroLocalEngine {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroEngineClock *clock;
  BroProblemService *problems;
  BroConfigService *config;
  BroScheduler *scheduler;
  BroLibraryService *libraries;
  BroDriveService *drives;
  BroJobService *jobs;
  BroRecoveryService *recovery;
  BroToolLocator *locator;
  BroEngineLog *log;
  BroEngineFacts *facts;
  BroEngineRelease release;
  gpointer release_data;
  /* On the executor: */
  GPtrArray *feeds; /* BroEventFeed * */
  BroSubscription *limits;
  BroInstant started_at;
  gboolean stopping;
  gboolean shut_down; /* the job service's stop has completed */
  gboolean capped;    /* stop's settle cap ran out */
  GSource *cap;       /* while stop settles */
};

static void engine_api_iface_init (BroEngineAPIInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroLocalEngine, bro_local_engine, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_ENGINE_API, engine_api_iface_init))

/* ---- the hop: every call runs on the executor ------------------------------------------------------------------ */

/* The shape of a call's done. */
typedef enum {
  ANSWER_PLAIN,
  ANSWER_STATUS,
  ANSWER_LIST,
  ANSWER_JOB,
  ANSWER_PAGE,
  ANSWER_DETAIL,
  ANSWER_LIBRARY,
  ANSWER_ENVELOPE,
  ANSWER_VALUE,
  ANSWER_FEED,
} Answer;

typedef struct _Call Call;
typedef void (*CallWork) (Call *call);

struct _Call {
  BroLocalEngine *self; /* a reference */
  CallWork work;
  Answer answer;
  GCallback done;
  gpointer data;
  GDestroyNotify destroy;
  /* the arguments */
  char *text;
  char *text2;
  char *text3;
  BroId id;
  gint64 number;
  gboolean flag;
  BroStepKind step;
  BroRipOptionsDto *options;
  BroJobQueryDto *query;
  BroJsonValue *doc;
  BroJobRequest *request;
  GArray *ids; /* BroId */
  /* what the work keeps between its steps */
  BroLibraryDto *library;
  BroBroError *error;
  BroEngineEvent *event;
};

static Call *
call_new (BroLocalEngine *self, CallWork work, Answer answer, GCallback done, gpointer data, GDestroyNotify destroy)
{
  Call *c = g_new0 (Call, 1);
  c->self = g_object_ref (self);
  c->work = work;
  c->answer = answer;
  c->done = done;
  c->data = data;
  c->destroy = destroy;
  return c;
}

static void
call_free (Call *c)
{
  if (c->destroy)
    c->destroy (c->data);
  g_free (c->text);
  g_free (c->text2);
  g_free (c->text3);
  bro_rip_options_dto_free (c->options);
  bro_job_query_dto_free (c->query);
  g_clear_pointer (&c->doc, bro_json_value_unref);
  bro_job_request_free (c->request);
  g_clear_pointer (&c->ids, g_array_unref);
  bro_library_dto_free (c->library);
  bro_bro_error_free (c->error);
  g_clear_pointer (&c->event, bro_engine_event_unref);
  g_object_unref (c->self);
  g_free (c);
}

/* The call's done with @error and no result, then the call is freed. */
static void
fail (Call *c, const BroBroError *error)
{
  switch (c->answer)
    {
    case ANSWER_PLAIN:
      ((BroEngineDone) c->done) (error, c->data);
      break;
    case ANSWER_STATUS:
      ((BroSystemStatusDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_LIST:
      ((BroEngineListDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_JOB:
      ((BroEngineJobDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_PAGE:
      ((BroEnginePageDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_DETAIL:
      ((BroEngineDetailDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_LIBRARY:
      ((BroEngineLibraryDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_ENVELOPE:
      ((BroEngineEnvelopeDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_VALUE:
      ((BroEngineValueDone) c->done) (NULL, error, c->data);
      break;
    case ANSWER_FEED:
      ((BroEngineFeedDone) c->done) (NULL, error, c->data);
      break;
    }
  call_free (c);
}

static BroBroError *
failure (BroMessageCode code, BroJsonValue *params)
{
  return bro_bro_error_new (bro_message_code_wire (code), params, NULL);
}

static void
fail_with (Call *c, BroMessageCode code, BroJsonValue *params)
{
  g_autoptr (BroBroError) e = failure (code, params);
  fail (c, e);
}

/* {key: value}; takes @value. */
static BroJsonValue *
param (const char *key, BroJsonValue *value)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, key, value);
  return o;
}

static void
succeeded (Call *c)
{
  ((BroEngineDone) c->done) (NULL, c->data);
  call_free (c);
}

/* A service's plain done, as the call's. */
static void
plain_done (const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (error)
    fail (c, error);
  else
    succeeded (c);
}

static void
call_run (gpointer data)
{
  Call *c = data;
  if (c->self->stopping)
    fail_with (c, BRO_MSG_SERVER_STOPPING, NULL);
  else
    c->work (c);
}

/* Runs the call's work on the executor; server.stopping once stop has begun (here when the executor no longer takes
 * posts). */
static void
hop (Call *c)
{
  if (!bro_engine_executor_post (c->self->executor, call_run, c, NULL))
    fail_with (c, BRO_MSG_SERVER_STOPPING, NULL);
}

/* Hands the call's done, data and destroy to a service whose done has the same shape. */
static void
forwarded (Call *c)
{
  c->destroy = NULL;
  call_free (c);
}

/* ---- start and stop ---------------------------------------------------------------------------------------------- */

static void
apply_limits (BroLocalEngine *self)
{
  g_autoptr (BroConfig) config = bro_config_service_current (self->config);
  g_autoptr (BroSchedulerLimits) limits = bro_scheduler_limits_of (config, self->facts->cores);
  bro_scheduler_update_limits (self->scheduler, limits);
}

static void
limits_changed (BroEngineEvent *event, gpointer data)
{
  apply_limits (data);
}

static void
start_jobs_started (const BroBroError *error, gpointer data)
{
  plain_done (error, data);
}

static void
start_drives_started (const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (error)
    fail (c, error);
  else
    bro_job_service_start (c->self->jobs, start_jobs_started, c, NULL);
}

static void
start_libraries_started (const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (error)
    fail (c, error);
  else
    bro_drive_service_start (c->self->drives, start_drives_started, c, NULL);
}

static void
start_recovered (BroBroMessage *summary, const BroBroError *error, gpointer data)
{
  Call *c = data;
  BroLocalEngine *self = c->self;
  BroEngineEventType changed = BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED;
  if (error)
    {
      fail (c, error);
      return;
    }
  if (summary)
    {
      bro_engine_log_log (self->log, summary);
      bro_bro_message_free (summary);
    }
  apply_limits (self);
  self->limits = bro_event_bus_subscribe (self->bus, &changed, 1, limits_changed, self, NULL);
  bro_library_service_start (self->libraries, start_libraries_started, c, NULL);
}

static void
start_loaded (const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (error)
    fail (c, error);
  else
    bro_recovery_service_recover (c->self->recovery, start_recovered, c, NULL);
}

static void
work_start (Call *c)
{
  c->self->started_at = bro_engine_clock_now (c->self->clock);
  bro_config_service_load (c->self->config, start_loaded, c, NULL);
}

void
bro_local_engine_start (BroLocalEngine *self, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (self, work_start, ANSWER_PLAIN, G_CALLBACK (done), data, destroy));
}

static void
stop_jobs_stopped (const BroBroError *error, gpointer data)
{
  BRO_LOCAL_ENGINE (data)->shut_down = TRUE;
}

static gboolean
stop_capped (gpointer data)
{
  BRO_LOCAL_ENGINE (data)->capped = TRUE;
  return G_SOURCE_REMOVE;
}

/* Two GLib timeouts on the executor's context (not actions: they aren't counted in the activity): this one waits for
 * the shutdown, then for the executor to go idle, both under one cap, the other: 30 s of real time (not the EngineClock:
 * the cap must run out while the engine winds down, whatever the clock is). Then the executor stops and the root's
 * release runs. */
static gboolean
stop_settle (gpointer data)
{
  Call *c = data;
  BroLocalEngine *self = c->self;
  if (!self->capped && (!self->shut_down || bro_engine_executor_activity (self->executor) > 0))
    return G_SOURCE_CONTINUE;
  g_source_destroy (self->cap);
  g_clear_pointer (&self->cap, g_source_unref);
  bro_engine_executor_stop (self->executor); /* on the executor: it ends after this */
  if (self->release)
    self->release (self->release_data);
  self->release = NULL;
  succeeded (c);
  return G_SOURCE_REMOVE;
}

/* The stop order (decision 170): from now on every call answers server.stopping; the feeds close and the intake stops
 * (drives, libraries); the runner shuts every live job down storing nothing more, so the store is what a killed engine
 * leaves; then, once no job is live and the executor is idle (at most 30 s), the executor stops and the root's release
 * runs. */
static void
work_stop (Call *c)
{
  BroLocalEngine *self = c->self;
  GSource *settle;
  self->stopping = TRUE;
  for (guint i = 0; i < self->feeds->len; i++)
    bro_event_feed_close (g_ptr_array_index (self->feeds, i));
  g_ptr_array_set_size (self->feeds, 0);
  if (self->limits)
    {
      bro_subscription_cancel (self->limits);
      g_clear_object (&self->limits);
    }
  bro_drive_service_stop (self->drives);
  bro_library_service_stop (self->libraries);
  bro_job_service_stop (self->jobs, stop_jobs_stopped, g_object_ref (self), g_object_unref);
  self->cap = g_timeout_source_new_seconds (SETTLE_CAP_S);
  g_source_set_callback (self->cap, stop_capped, self, NULL);
  g_source_attach (self->cap, g_main_context_get_thread_default ());
  settle = g_timeout_source_new (SETTLE_POLL_MS);
  g_source_set_callback (settle, stop_settle, c, NULL);
  g_source_attach (settle, g_main_context_get_thread_default ());
  g_source_unref (settle);
}

static void
api_stop (BroEngineAPI *api, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_stop, ANSWER_PLAIN, G_CALLBACK (done), data, destroy));
}

static void
work_start_job (Call *c)
{
  bro_job_service_create (c->self->jobs, c->request, (BroJobDtoDone) c->done, c->data, c->destroy);
  forwarded (c);
}

void
bro_local_engine_start_job (BroLocalEngine *self, const BroJobRequest *request, BroEngineJobDone done, gpointer data,
                            GDestroyNotify destroy)
{
  Call *c = call_new (self, work_start_job, ANSWER_JOB, G_CALLBACK (done), data, destroy);
  c->request = bro_job_request_copy (request);
  hop (c);
}

/* ---- the system and the snapshot --------------------------------------------------------------------------------- */

/* The status (taken), or NULL and @error. */
typedef void (*StatusReady) (BroSystemStatusDto *status, const BroBroError *error, gpointer data);

typedef struct {
  BroLocalEngine *self; /* a reference */
  StatusReady ready;
  gpointer data;
  BroToolLocator *locator;
  BroToolInfo *makemkvcon;
  BroToolInfo *mkvmerge;
} Status;

static void
status_free (gpointer data)
{
  Status *s = data;
  bro_tool_info_free (s->makemkvcon);
  bro_tool_info_free (s->mkvmerge);
  g_object_unref (s->locator);
  g_object_unref (s->self);
  g_free (s);
}

static gboolean
status_locate (gpointer data, BroBroError **error)
{
  Status *s = data;
  s->makemkvcon = bro_tool_locator_locate (s->locator, BRO_TOOL_KIND_MAKEMKVCON);
  s->mkvmerge = bro_tool_locator_locate (s->locator, BRO_TOOL_KIND_MKVMERGE);
  return TRUE;
}

static void
status_located (gpointer data, const BroBroError *error)
{
  Status *s = data;
  BroLocalEngine *self = s->self;
  BroSystemStatusDto *status;
  if (error)
    {
      s->ready (NULL, error, s->data);
      return;
    }
  status = bro_system_status_dto_new ();
  status->facts = bro_engine_facts_copy (self->facts);
  status->started_at = self->started_at;
  status->makemkv_found = s->makemkvcon->path != NULL;
  status->makemkv_version = g_strdup (s->makemkvcon->version);
  g_ptr_array_add (status->tools, g_steal_pointer (&s->makemkvcon));
  g_ptr_array_add (status->tools, g_steal_pointer (&s->mkvmerge));
  g_ptr_array_extend_and_steal (status->problems, bro_problem_service_problems (self->problems));
  g_ptr_array_extend_and_steal (status->libraries, bro_library_service_libraries (self->libraries));
  status->config_version = bro_config_service_version (self->config);
  status->event_seq = bro_event_bus_last_seq (self->bus); /* read after the worker returns */
  s->ready (status, NULL, s->data);
}

/* The system's status: the tools are located on a worker; @ready runs on the executor. */
static void
build_status (BroLocalEngine *self, StatusReady ready, gpointer data)
{
  Status *s = g_new0 (Status, 1);
  s->self = g_object_ref (self);
  s->ready = ready;
  s->data = data;
  s->locator = g_object_ref (self->locator);
  bro_engine_executor_worker (self->executor, status_locate, status_located, s, status_free);
}

static void
system_ready (BroSystemStatusDto *status, const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (!status)
    {
      fail (c, error);
      return;
    }
  ((BroSystemStatusDone) c->done) (status, NULL, c->data);
  bro_system_status_dto_free (status);
  call_free (c);
}

static void
work_system (Call *c)
{
  build_status (c->self, system_ready, c);
}

static void
api_system (BroEngineAPI *api, BroSystemStatusDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_system, ANSWER_STATUS, G_CALLBACK (done), data, destroy));
}

/* The snapshot event (taken), or NULL and @error. */
typedef void (*GatherReady) (BroEngineEvent *event, const BroBroError *error, gpointer data);

typedef struct {
  BroLocalEngine *self; /* a reference */
  GatherReady ready;
  gpointer data;
  gint64 seq;
  BroSnapshotDto *snapshot;
  gboolean finished; /* the finished jobs are asked for */
} Gather;

static void
gather_free (Gather *g)
{
  bro_snapshot_dto_free (g->snapshot);
  g_object_unref (g->self);
  g_free (g);
}

static void
gather_failed (Gather *g, const BroBroError *error)
{
  g->ready (NULL, error, g->data);
  gather_free (g);
}

static void
gather_status (BroSystemStatusDto *status, const BroBroError *error, gpointer data)
{
  Gather *g = data;
  BroLocalEngine *self = g->self;
  BroSnapshotDto *s = g->snapshot;
  g_autoptr (BroJsonValue) json = NULL;
  BroEngineEvent *event;
  if (!status)
    {
      gather_failed (g, error);
      return;
    }
  status->event_seq = g->seq;
  s->system = status;
  g_ptr_array_extend_and_steal (s->drives, bro_drive_service_drives (self->drives));
  bro_queue_orders_free (s->queues);
  s->queues = bro_scheduler_queues (self->scheduler);
  g_ptr_array_extend_and_steal (s->problems, bro_problem_service_problems (self->problems));
  g_ptr_array_extend_and_steal (s->libraries, bro_library_service_libraries (self->libraries));
  s->config_version = bro_config_service_version (self->config);
  json = bro_snapshot_dto_json (s);
  event = bro_engine_event_new (g->seq, bro_engine_clock_now (self->clock), BRO_ENGINE_EVENT_TYPE_SNAPSHOT, json);
  g->ready (event, NULL, g->data);
  gather_free (g);
}

static void gather_page (const BroJobPageDto *page, const BroBroError *error, gpointer data);

/* Asks for the next page of unfinished jobs (after @cursor), or for the finished ones. */
static void
gather_query (Gather *g, const char *cursor)
{
  static const BroJobState unfinished[] = { BRO_JOB_STATE_QUEUED, BRO_JOB_STATE_WAITING_FOR_RESOURCES, BRO_JOB_STATE_RUNNING,
                                            BRO_JOB_STATE_BLOCKED, BRO_JOB_STATE_AWAITING_DECISION };
  static const BroJobState finished[] = { BRO_JOB_STATE_FINISHED };
  g_autoptr (BroJobQueryDto) query = bro_job_query_dto_new ();
  if (g->finished)
    {
      g_array_append_vals (query->states, finished, G_N_ELEMENTS (finished));
      query->limit = FINISHED_IN_SNAPSHOT;
    }
  else
    {
      g_array_append_vals (query->states, unfinished, G_N_ELEMENTS (unfinished));
      query->cursor = g_strdup (cursor);
      query->limit = BRO_JOB_QUERY_DTO_MAX_LIMIT;
    }
  bro_job_service_query (g->self->jobs, query, gather_page, g, NULL);
}

static void
gather_page (const BroJobPageDto *page, const BroBroError *error, gpointer data)
{
  Gather *g = data;
  if (!page)
    {
      gather_failed (g, error);
      return;
    }
  for (guint i = 0; i < page->items->len; i++)
    g_ptr_array_add (g->snapshot->jobs, bro_job_dto_copy (g_ptr_array_index (page->items, i)));
  if (!g->finished && page->next)
    {
      gather_query (g, page->next);
      return;
    }
  if (!g->finished)
    {
      g->finished = TRUE;
      gather_query (g, NULL);
      return;
    }
  build_status (g->self, gather_status, g);
}

/* The snapshot event (never published): its seq is the last event's when gathering began (decision 186). */
static void
gather (BroLocalEngine *self, GatherReady ready, gpointer data)
{
  Gather *g = g_new0 (Gather, 1);
  g->self = g_object_ref (self);
  g->ready = ready;
  g->data = data;
  g->seq = bro_event_bus_last_seq (self->bus);
  g->snapshot = bro_snapshot_dto_new ();
  gather_query (g, NULL);
}

typedef struct {
  BroSnapshotReady ready;
  gpointer data;
} Resync;

static void
resync_gathered (BroEngineEvent *event, const BroBroError *error, gpointer data)
{
  Resync *r = data;
  r->ready (event, r->data);
  g_free (r);
}

/* The feed's snapshot after a gap, on the executor; none once stop has begun (the feed then closes). */
static void
resync (GObject *owner, BroSnapshotReady ready, gpointer data)
{
  BroLocalEngine *self = BRO_LOCAL_ENGINE (owner);
  Resync *r;
  if (self->stopping)
    {
      ready (NULL, data);
      return;
    }
  r = g_new0 (Resync, 1);
  r->ready = ready;
  r->data = data;
  gather (self, resync_gathered, r);
}

/* ---- drives -------------------------------------------------------------------------------------------------------- */

static void
work_drives (Call *c)
{
  g_autoptr (GPtrArray) drives = bro_drive_service_drives (c->self->drives);
  ((BroEngineListDone) c->done) (drives, NULL, c->data);
  call_free (c);
}

static void
api_drives (BroEngineAPI *api, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_drives, ANSWER_LIST, G_CALLBACK (done), data, destroy));
}

static void
work_eject (Call *c)
{
  bro_drive_service_eject (c->self->drives, c->text, plain_done, c, NULL);
}

static void
api_eject (BroEngineAPI *api, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_eject, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->text = g_strdup (drive_id);
  hop (c);
}

static void
work_close_tray (Call *c)
{
  bro_drive_service_close_tray (c->self->drives, c->text, plain_done, c, NULL);
}

static void
api_close_tray (BroEngineAPI *api, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_close_tray, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->text = g_strdup (drive_id);
  hop (c);
}

static void
work_close_all_trays (Call *c)
{
  bro_drive_service_close_all (c->self->drives, plain_done, c, NULL);
}

static void
api_close_all_trays (BroEngineAPI *api, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_close_all_trays, ANSWER_PLAIN, G_CALLBACK (done), data, destroy));
}

/* @path without the separators at its end. */
static char *
trimmed (const char *path)
{
  char *p = g_strdup (path);
  gsize n = strlen (p);
  while (n > 0 && (p[n - 1] == '/' || p[n - 1] == '\\'))
    p[--n] = '\0';
  return p;
}

static gboolean
same_path (const char *a, const char *b)
{
  g_autofree char *x = trimmed (a);
  g_autofree char *y = trimmed (b);
  return strcmp (x, y) == 0;
}

static void
work_rip (Call *c)
{
  BroLocalEngine *self = c->self;
  const BroRipOptionsDto *options = c->options;
  g_autoptr (GPtrArray) drives = bro_drive_service_drives (self->drives);
  const BroDriveDto *drive = NULL;
  g_autofree char *library = g_strdup (options->library);
  g_autoptr (BroJobRequest) request = NULL;
  BroJsonValue *chosen;
  for (guint i = 0; i < drives->len && !drive; i++)
    {
      const BroDriveDto *d = g_ptr_array_index (drives, i);
      if (strcmp (d->id, c->text) == 0 && d->connected)
        drive = d;
    }
  if (!drive)
    {
      fail_with (c, BRO_MSG_DRIVE_NOT_FOUND, param ("driveId", bro_json_value_new_string (c->text)));
      return;
    }
  if (!drive->medium)
    {
      fail_with (c, BRO_MSG_DRIVE_NO_DISC, NULL);
      return;
    }
  if (drive->busy)
    {
      fail_with (c, BRO_MSG_DRIVE_BUSY, NULL);
      return;
    }
  if (drive->medium->content == BRO_DISC_CONTENT_AUDIO || drive->medium->content == BRO_DISC_CONTENT_DATA)
    {
      fail_with (c, BRO_MSG_DISC_NOT_VIDEO, param ("drive", bro_json_value_new_string (drive->name)));
      return;
    }
  if (options->titles)
    {
      fail_with (c, BRO_MSG_SESSION_NOT_OPEN, NULL);
      return;
    }
  if (options->output_folder)
    {
      g_autoptr (GPtrArray) libraries = bro_library_service_libraries (self->libraries);
      g_clear_pointer (&library, g_free);
      for (guint i = 0; i < libraries->len && !library; i++)
        {
          const BroLibraryDto *l = g_ptr_array_index (libraries, i);
          if (same_path (l->path, options->output_folder))
            library = g_strdup (l->id);
        }
      if (!library)
        {
          fail_with (c, BRO_MSG_LIBRARY_NOT_ATTACHED, param ("path", bro_json_value_new_string (options->output_folder)));
          return;
        }
    }
  chosen = bro_json_value_new_object ();
  if (options->has_mode)
    bro_json_value_set (chosen, "mode", bro_json_value_new_string (bro_rip_mode_to_wire (options->mode)));
  if (options->profile)
    bro_json_value_set (chosen, "profile", bro_json_value_new_string (options->profile));
  if (library)
    bro_json_value_set (chosen, "library", bro_json_value_new_string (library));
  request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, *drive->medium->label ? drive->medium->label : drive->name);
  request->drive_id = g_strdup (drive->id);
  request->has_media_generation = TRUE;
  request->media_generation = drive->medium->generation;
  request->options = chosen;
  bro_job_service_create (self->jobs, request, (BroJobDtoDone) c->done, c->data, c->destroy);
  forwarded (c);
}

static void
api_rip (BroEngineAPI *api, const char *drive_id, const BroRipOptionsDto *options, BroEngineJobDone done, gpointer data,
         GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_rip, ANSWER_JOB, G_CALLBACK (done), data, destroy);
  c->text = g_strdup (drive_id);
  c->options = bro_rip_options_dto_copy (options);
  hop (c);
}

/* ---- jobs ---------------------------------------------------------------------------------------------------------- */

static void
work_jobs (Call *c)
{
  bro_job_service_query (c->self->jobs, c->query, (BroJobPageDone) c->done, c->data, c->destroy);
  forwarded (c);
}

static BroJobQueryDto *
query_copy (const BroJobQueryDto *query)
{
  BroJobQueryDto *q = bro_job_query_dto_new ();
  g_array_append_vals (q->states, query->states->data, query->states->len);
  g_array_append_vals (q->outcomes, query->outcomes->data, query->outcomes->len);
  q->has_kind = query->has_kind;
  q->kind = query->kind;
  q->has_queue = query->has_queue;
  q->queue = query->queue;
  q->drive_id = g_strdup (query->drive_id);
  q->has_unit_id = query->has_unit_id;
  q->unit_id = query->unit_id;
  q->cursor = g_strdup (query->cursor);
  q->limit = query->limit;
  return q;
}

static void
api_jobs (BroEngineAPI *api, const BroJobQueryDto *query, BroEnginePageDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_jobs, ANSWER_PAGE, G_CALLBACK (done), data, destroy);
  c->query = query_copy (query);
  hop (c);
}

static void
work_job (Call *c)
{
  bro_job_service_job (c->self->jobs, &c->id, (BroJobDetailDone) c->done, c->data, c->destroy);
  forwarded (c);
}

static void
api_job (BroEngineAPI *api, const BroId *job_id, BroEngineDetailDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_job, ANSWER_DETAIL, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  hop (c);
}

static void
work_job_log (Call *c)
{
  bro_job_service_log (c->self->jobs, &c->id, c->number, (BroLogLinesDone) c->done, c->data, c->destroy);
  forwarded (c);
}

static void
api_job_log (BroEngineAPI *api, const BroId *job_id, gint64 after, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_job_log, ANSWER_LIST, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  c->number = after;
  hop (c);
}

static void
work_cancel (Call *c)
{
  bro_job_service_cancel (c->self->jobs, &c->id, plain_done, c, NULL);
}

static void
api_cancel (BroEngineAPI *api, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_cancel, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  hop (c);
}

static void
work_retry (Call *c)
{
  bro_job_service_retry (c->self->jobs, &c->id, c->flag ? &c->step : NULL, (BroJobDtoDone) c->done, c->data, c->destroy);
  forwarded (c);
}

static void
api_retry (BroEngineAPI *api, const BroId *job_id, const BroStepKind *from_step, BroEngineJobDone done, gpointer data,
           GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_retry, ANSWER_JOB, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  c->flag = from_step != NULL;
  if (from_step)
    c->step = *from_step;
  hop (c);
}

static void
keep_apart_queued (const BroJobDto *job, const BroBroError *error, gpointer data)
{
  plain_done (error, data);
}

static void
work_keep_apart (Call *c)
{
  bro_job_service_keep_apart (c->self->jobs, &c->id, keep_apart_queued, c, NULL);
}

static void
api_keep_apart (BroEngineAPI *api, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_keep_apart, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  hop (c);
}

static void
work_decide (Call *c)
{
  bro_job_service_decide (c->self->jobs, &c->id, c->text, c->text2, plain_done, c, NULL);
}

static void
api_decide (BroEngineAPI *api, const BroId *job_id, const char *decision_id, const char *answer, BroEngineDone done, gpointer data,
            GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_decide, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  c->text = g_strdup (decision_id);
  c->text2 = g_strdup (answer);
  hop (c);
}

static void
work_move (Call *c)
{
  bro_job_service_move (c->self->jobs, &c->id, (int) c->number, plain_done, c, NULL);
}

static void
api_move (BroEngineAPI *api, const BroId *job_id, int by, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_move, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  c->number = by;
  hop (c);
}

static void
work_start_now (Call *c)
{
  bro_job_service_start_now (c->self->jobs, &c->id, plain_done, c, NULL);
}

static void
api_start_now (BroEngineAPI *api, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_start_now, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->id = *job_id;
  hop (c);
}

/* ---- libraries ----------------------------------------------------------------------------------------------------- */

static void
work_libraries (Call *c)
{
  g_autoptr (GPtrArray) libraries = bro_library_service_libraries (c->self->libraries);
  ((BroEngineListDone) c->done) (libraries, NULL, c->data);
  call_free (c);
}

static void
api_libraries (BroEngineAPI *api, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_libraries, ANSWER_LIST, G_CALLBACK (done), data, destroy));
}

/* The last part of a path, ignoring separators at its end. */
static char *
last_part (const char *path)
{
  g_autofree char *p = trimmed (path);
  const char *slash = NULL;
  for (const char *x = p; *x; x++)
    if (*x == '/' || *x == '\\')
      slash = x;
  return g_strdup (slash ? slash + 1 : p);
}

/* The name as an EntryId (common.json: letters, digits, '.', '_', '-'; at most 64): ASCII letters and digits
 * lowercased, every run of anything else one '-', no '-' at either end, at most 60 characters; "library" when nothing
 * is left. Then -2, -3 … until no library of @config has it (ignoring case; EntryIds are ASCII). */
static char *
new_library_id (const char *name, const BroConfig *config)
{
  g_autoptr (GString) id = g_string_new ("");
  g_autoptr (GPtrArray) taken = bro_config_libraries (config);
  char *candidate;
  for (const char *x = name; *x; x++)
    {
      char ch = *x;
      if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
        g_string_append_c (id, ch);
      else if (ch >= 'A' && ch <= 'Z')
        g_string_append_c (id, (char) (ch + 32));
      else if (id->len > 0 && id->str[id->len - 1] != '-')
        g_string_append_c (id, '-');
    }
  while (id->len > 0 && id->str[id->len - 1] == '-')
    g_string_truncate (id, id->len - 1);
  if (id->len > 60)
    {
      g_string_truncate (id, 60);
      while (id->len > 0 && id->str[id->len - 1] == '-')
        g_string_truncate (id, id->len - 1);
    }
  if (id->len == 0)
    g_string_assign (id, "library");
  candidate = g_strdup (id->str);
  for (int n = 2;; n++)
    {
      gboolean used = FALSE;
      for (guint i = 0; i < taken->len && !used; i++)
        used = g_ascii_strcasecmp (((BroLibraryEntry *) g_ptr_array_index (taken, i))->id, candidate) == 0;
      if (!used)
        return candidate;
      g_free (candidate);
      candidate = g_strdup_printf ("%s-%d", id->str, n);
    }
}

static void
attach_taken_back (const BroBroError *error, gpointer data)
{
  Call *c = data;
  fail (c, c->error);
}

static void
attach_saved (BroConfigEnvelope *envelope, const BroBroError *error, gpointer data)
{
  Call *c = data;
  BroLocalEngine *self = c->self;
  g_autoptr (GPtrArray) libraries = NULL;
  const BroLibraryDto *found = c->library;
  bro_config_envelope_free (envelope);
  if (error)
    {
      c->error = bro_bro_error_copy (error);
      bro_library_service_remove_marker (self->libraries, c->text3, attach_taken_back, c, NULL);
      return;
    }
  libraries = bro_library_service_libraries (self->libraries);
  for (guint i = 0; i < libraries->len; i++)
    if (strcmp (((BroLibraryDto *) g_ptr_array_index (libraries, i))->id, c->text3) == 0)
      found = g_ptr_array_index (libraries, i);
  ((BroEngineLibraryDone) c->done) (found, NULL, c->data);
  call_free (c);
}

static void
attach_attached (BroLibraryDto *library, const BroBroError *error, gpointer data)
{
  Call *c = data;
  BroLocalEngine *self = c->self;
  g_autoptr (BroConfig) current = NULL;
  g_autoptr (BroJsonValue) doc = NULL;
  BroJsonValue *entry, *libraries, *had;
  if (!library)
    {
      fail (c, error);
      return;
    }
  c->library = library;
  current = bro_config_service_current (self->config);
  doc = bro_json_value_new_object ();
  for (guint k = 0; current->document->kind == BRO_JSON_VALUE_OBJECT && k < current->document->keys->len; k++)
    if (strcmp (current->document->keys->pdata[k], "libraries") != 0)
      bro_json_value_set (doc, current->document->keys->pdata[k], bro_json_value_ref (current->document->items->pdata[k]));
  libraries = bro_json_value_new_array ();
  had = bro_json_value_member (current->document, "libraries");
  for (guint i = 0; had && i < bro_json_value_length (had); i++)
    bro_json_value_append (libraries, bro_json_value_ref (bro_json_value_at (had, i)));
  entry = bro_json_value_new_object ();
  bro_json_value_set (entry, "id", bro_json_value_new_string (c->text3));
  bro_json_value_set (entry, "name", bro_json_value_new_string (library->name));
  bro_json_value_set (entry, "path", bro_json_value_new_string (c->text));
  bro_json_value_append (libraries, entry);
  bro_json_value_set (doc, "libraries", libraries);
  bro_config_service_update (self->config, doc, bro_config_service_version (self->config), attach_saved, c, NULL);
}

/* The configured library's marker is written: the library as LibraryService lists it now, else what attach gave. */
static void
attach_marked (BroLibraryDto *library, const BroBroError *error, gpointer data)
{
  Call *c = data;
  g_autoptr (GPtrArray) libraries = NULL;
  const BroLibraryDto *found = library;
  if (!library)
    {
      fail (c, error);
      return;
    }
  c->library = library;
  libraries = bro_library_service_libraries (c->self->libraries);
  for (guint i = 0; i < libraries->len; i++)
    if (strcmp (((BroLibraryDto *) g_ptr_array_index (libraries, i))->id, c->text3) == 0)
      found = g_ptr_array_index (libraries, i);
  ((BroEngineLibraryDone) c->done) (found, NULL, c->data);
  call_free (c);
}

/* The id is the name (else the folder's last part) made into an EntryId, with -2, -3 … when the configuration has it
 * already (decision 187). LibraryService writes the marker, then the configuration gets {id, name, path} at its
 * current version; if that save fails the marker is removed and the save's error returned, so a retry doesn't meet
 * library.alreadyAttached. The path of a library the configuration already has (the user confirming a folder without
 * its marker, H1-7): the marker is written under that library's id and name, and the configuration is left as it is. */
static void
work_attach_library (Call *c)
{
  BroLocalEngine *self = c->self;
  g_autoptr (BroConfig) current = bro_config_service_current (self->config);
  g_autoptr (GPtrArray) configured = bro_config_libraries (current);
  for (guint i = 0; i < configured->len; i++)
    {
      const BroLibraryEntry *l = g_ptr_array_index (configured, i);
      if (strcmp (l->path, c->text) == 0)
        {
          c->text3 = g_strdup (l->id);
          bro_library_service_attach (self->libraries, l->id, c->text, l->name, attach_marked, c, NULL);
          return;
        }
    }
  g_autofree char *display = *c->text2 ? g_strdup (c->text2) : last_part (c->text);
  c->text3 = new_library_id (display, current);
  bro_library_service_attach (self->libraries, c->text3, c->text, c->text2, attach_attached, c, NULL);
}

static void
api_attach_library (BroEngineAPI *api, const char *path, const char *name, BroEngineLibraryDone done, gpointer data,
                    GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_attach_library, ANSWER_LIBRARY, G_CALLBACK (done), data, destroy);
  c->text = g_strdup (path);
  c->text2 = g_strdup (name ? name : "");
  hop (c);
}

/* ---- configuration ------------------------------------------------------------------------------------------------- */

static void
envelope_done (BroConfigEnvelope *envelope, const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (!envelope)
    {
      fail (c, error);
      return;
    }
  ((BroEngineEnvelopeDone) c->done) (envelope, NULL, c->data);
  bro_config_envelope_free (envelope);
  call_free (c);
}

static void
work_config (Call *c)
{
  bro_config_service_envelope (c->self->config, envelope_done, c, NULL);
}

static void
api_config (BroEngineAPI *api, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_config, ANSWER_ENVELOPE, G_CALLBACK (done), data, destroy));
}

static void
work_update_config (Call *c)
{
  bro_config_service_update (c->self->config, c->doc, c->number, envelope_done, c, NULL);
}

static void
api_update_config (BroEngineAPI *api, const BroJsonValue *doc, gint64 version, BroEngineEnvelopeDone done, gpointer data,
                   GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_update_config, ANSWER_ENVELOPE, G_CALLBACK (done), data, destroy);
  c->doc = bro_json_value_ref ((BroJsonValue *) doc);
  c->number = version;
  hop (c);
}

static void
work_validate_config (Call *c)
{
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GPtrArray) issues = bro_config_service_validate (c->self->config, c->doc, &error);
  if (!issues)
    {
      fail (c, error);
      return;
    }
  ((BroEngineListDone) c->done) (issues, NULL, c->data);
  call_free (c);
}

static void
api_validate_config (BroEngineAPI *api, const BroJsonValue *doc, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_validate_config, ANSWER_LIST, G_CALLBACK (done), data, destroy);
  c->doc = bro_json_value_ref ((BroJsonValue *) doc);
  hop (c);
}

static void
work_set_secret (Call *c)
{
  bro_config_service_set_secret (c->self->config, c->text, c->text2, plain_done, c, NULL);
}

static void
api_set_secret (BroEngineAPI *api, const char *name, const char *value, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_set_secret, ANSWER_PLAIN, G_CALLBACK (done), data, destroy);
  c->text = g_strdup (name);
  c->text2 = g_strdup (value);
  hop (c);
}

static void
work_export_config (Call *c)
{
  g_autoptr (GBytes) bytes = bro_config_service_export (c->self->config);
  gsize size;
  const char *text = g_bytes_get_data (bytes, &size);
  g_autoptr (BroJsonValue) bundle = bro_json_value_parse (text, (gssize) size);
  ((BroEngineValueDone) c->done) (bundle, NULL, c->data);
  call_free (c);
}

static void
api_export_config (BroEngineAPI *api, BroEngineValueDone done, gpointer data, GDestroyNotify destroy)
{
  hop (call_new (BRO_LOCAL_ENGINE (api), work_export_config, ANSWER_VALUE, G_CALLBACK (done), data, destroy));
}

/* ---- events -------------------------------------------------------------------------------------------------------- */

static void
events_open (Call *c, BroEventStream *stream, BroEngineEvent *first)
{
  BroLocalEngine *self = c->self;
  g_autoptr (BroEventFeed) feed = _bro_event_feed_new (self->executor, stream, (const BroId *) c->ids->data, c->ids->len, first, resync,
                                                       G_OBJECT (self));
  for (guint i = self->feeds->len; i > 0; i--)
    if (_bro_event_feed_is_closed (g_ptr_array_index (self->feeds, i - 1)))
      g_ptr_array_remove_index (self->feeds, i - 1);
  g_ptr_array_add (self->feeds, g_object_ref (feed));
  ((BroEngineFeedDone) c->done) (feed, NULL, c->data);
  call_free (c);
}

static void
events_gathered (BroEngineEvent *event, const BroBroError *error, gpointer data)
{
  Call *c = data;
  if (!event)
    {
      fail (c, error);
      return;
    }
  if (c->self->stopping)
    {
      bro_engine_event_unref (event);
      fail_with (c, BRO_MSG_SERVER_STOPPING, NULL);
      return;
    }
  events_open (c, bro_event_bus_stream (c->self->bus, TRUE, event->seq, NULL, 0), event);
}

/* Decision 168, 186: with an after the feed replays from it (one the bus can't replay starts with a resync, which the
 * feed answers with a snapshot); without one the first event is a snapshot whose seq is the last event's before the
 * snapshot was gathered, and the events published while it was gathered follow it as deltas. */
static void
work_events (Call *c)
{
  if (c->flag)
    events_open (c, bro_event_bus_stream (c->self->bus, TRUE, c->number, NULL, 0), NULL);
  else
    gather (c->self, events_gathered, c);
}

static void
api_events (BroEngineAPI *api, gboolean has_after, gint64 after, const BroId *job_log, gsize n_job_log, BroEngineFeedDone done,
            gpointer data, GDestroyNotify destroy)
{
  Call *c = call_new (BRO_LOCAL_ENGINE (api), work_events, ANSWER_FEED, G_CALLBACK (done), data, destroy);
  c->flag = has_after;
  c->number = after;
  c->ids = g_array_new (FALSE, FALSE, sizeof (BroId));
  g_array_append_vals (c->ids, job_log, (guint) n_job_log);
  hop (c);
}

/* ---- the object ---------------------------------------------------------------------------------------------------- */

BroLocalEngine *
bro_local_engine_new (BroEngineExecutor *executor, BroEventBus *bus, BroEngineClock *clock, BroProblemService *problems,
                      BroConfigService *config, BroScheduler *scheduler, BroLibraryService *libraries, BroDriveService *drives,
                      BroJobService *jobs, BroRecoveryService *recovery, BroToolLocator *locator, BroEngineLog *log,
                      const BroEngineFacts *facts, BroEngineRelease release, gpointer release_data)
{
  BroLocalEngine *self = g_object_new (BRO_TYPE_LOCAL_ENGINE, NULL);
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->clock = g_object_ref (clock);
  self->problems = g_object_ref (problems);
  self->config = g_object_ref (config);
  self->scheduler = g_object_ref (scheduler);
  self->libraries = g_object_ref (libraries);
  self->drives = g_object_ref (drives);
  self->jobs = g_object_ref (jobs);
  self->recovery = g_object_ref (recovery);
  self->locator = g_object_ref (locator);
  self->log = g_object_ref (log);
  self->facts = bro_engine_facts_copy (facts);
  self->release = release;
  self->release_data = release_data;
  return self;
}

static void
bro_local_engine_finalize (GObject *object)
{
  BroLocalEngine *self = BRO_LOCAL_ENGINE (object);
  if (self->limits)
    {
      bro_subscription_cancel (self->limits);
      g_object_unref (self->limits);
    }
  if (self->cap)
    {
      g_source_destroy (self->cap);
      g_source_unref (self->cap);
    }
  g_ptr_array_unref (self->feeds);
  g_object_unref (self->executor);
  g_object_unref (self->bus);
  g_object_unref (self->clock);
  g_object_unref (self->problems);
  g_object_unref (self->config);
  g_object_unref (self->scheduler);
  g_object_unref (self->libraries);
  g_object_unref (self->drives);
  g_object_unref (self->jobs);
  g_object_unref (self->recovery);
  g_object_unref (self->locator);
  g_object_unref (self->log);
  bro_engine_facts_free (self->facts);
  G_OBJECT_CLASS (bro_local_engine_parent_class)->finalize (object);
}

static void
bro_local_engine_class_init (BroLocalEngineClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_local_engine_finalize;
}

static void
bro_local_engine_init (BroLocalEngine *self)
{
  self->feeds = g_ptr_array_new_with_free_func (g_object_unref);
}

static void
engine_api_iface_init (BroEngineAPIInterface *iface)
{
  iface->system_status = api_system;
  iface->stop = api_stop;
  iface->drives = api_drives;
  iface->eject = api_eject;
  iface->close_tray = api_close_tray;
  iface->close_all_trays = api_close_all_trays;
  iface->rip = api_rip;
  iface->jobs = api_jobs;
  iface->job = api_job;
  iface->job_log = api_job_log;
  iface->cancel = api_cancel;
  iface->retry = api_retry;
  iface->keep_apart = api_keep_apart;
  iface->decide = api_decide;
  iface->move = api_move;
  iface->start_now = api_start_now;
  iface->libraries = api_libraries;
  iface->attach_library = api_attach_library;
  iface->config = api_config;
  iface->update_config = api_update_config;
  iface->validate_config = api_validate_config;
  iface->set_secret = api_set_secret;
  iface->export_config = api_export_config;
  iface->events = api_events;
}
