/* test-recovery.inc: shared/fixtures/engine/recovery-service.cases.json, on a real executor, event bus, problem service,
 * scheduler and library service, with the library harness's file tree and database, and jobs and steps in memory
 * (included by test-engine.c after test-library.inc). */
#include "bro-job-log.h"
#include "bro-message-renderer.h"
#include "bro-recovery-service.h"
#include "bro-settings-isolation.h"

/* ---- a step that only says what it can do after a stop ---- */

#define TEST_TYPE_RECOVERY_STEP (test_recovery_step_get_type ())
G_DECLARE_FINAL_TYPE (TestRecoveryStep, test_recovery_step, TEST, RECOVERY_STEP, GObject)

struct _TestRecoveryStep {
  GObject parent_instance;
  BroStepKind kind;
  BroResumability resumability;
};

static void recovery_step_iface_init (BroStepInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestRecoveryStep, test_recovery_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, recovery_step_iface_init))

static BroStepKind recovery_step_kind (BroStep *s) { return TEST_RECOVERY_STEP (s)->kind; }
static BroResumability recovery_step_resumability (BroStep *s) { return TEST_RECOVERY_STEP (s)->resumability; }
/* A resumeFrom step resumes from a "backup" checkpoint. */
static const char *recovery_step_resume_checkpoint (BroStep *s)
{
  return TEST_RECOVERY_STEP (s)->resumability == BRO_RESUMABILITY_RESUME_FROM ? "backup" : NULL;
}
static void test_recovery_step_class_init (TestRecoveryStepClass *k) {}
static void test_recovery_step_init (TestRecoveryStep *s) {}

static void
recovery_step_iface_init (BroStepInterface *iface)
{
  iface->kind = recovery_step_kind;
  iface->resumability = recovery_step_resumability;
  iface->resume_checkpoint = recovery_step_resume_checkpoint;
}

/* ---- a SettingsIsolation that records recover and returns what the case gives ---- */

#define TEST_TYPE_RECOVERY_ISOLATION (test_recovery_isolation_get_type ())
G_DECLARE_FINAL_TYPE (TestRecoveryIsolation, test_recovery_isolation, TEST, RECOVERY_ISOLATION, GObject)

struct _TestRecoveryIsolation {
  GObject parent_instance;
  GMutex lock;
  GPtrArray *log;      /* char * */
  GPtrArray *problems; /* BroBroMessage */
};

static void recovery_isolation_iface_init (BroSettingsIsolationInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestRecoveryIsolation, test_recovery_isolation, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_SETTINGS_ISOLATION, recovery_isolation_iface_init))

static GPtrArray *
recovery_isolation_recover (BroSettingsIsolation *i, GPtrArray *work_directories)
{
  TestRecoveryIsolation *self = TEST_RECOVERY_ISOLATION (i);
  GString *line = g_string_new ("recover");
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  for (guint n = 0; n < work_directories->len; n++)
    g_string_append_printf (line, " %s", (const char *) work_directories->pdata[n]);
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->log, g_string_free (line, FALSE));
  g_mutex_unlock (&self->lock);
  for (guint n = 0; n < self->problems->len; n++)
    g_ptr_array_add (out, bro_bro_message_copy (self->problems->pdata[n]));
  return out;
}

static void
recovery_isolation_iface_init (BroSettingsIsolationInterface *iface)
{
  iface->recover = recovery_isolation_recover;
}

static void
test_recovery_isolation_finalize (GObject *o)
{
  TestRecoveryIsolation *self = TEST_RECOVERY_ISOLATION (o);
  g_ptr_array_unref (self->log);
  g_ptr_array_unref (self->problems);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_recovery_isolation_parent_class)->finalize (o);
}

static void test_recovery_isolation_class_init (TestRecoveryIsolationClass *k) { G_OBJECT_CLASS (k)->finalize = test_recovery_isolation_finalize; }

static void
test_recovery_isolation_init (TestRecoveryIsolation *self)
{
  g_mutex_init (&self->lock);
  self->log = g_ptr_array_new_with_free_func (g_free);
  self->problems = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
}

/* ---- a MessageRenderer: '<code>' then ' <key>=<value>' per param (strings as they are, other values as JSON) ---- */

#define TEST_TYPE_RECOVERY_RENDERER (test_recovery_renderer_get_type ())
G_DECLARE_FINAL_TYPE (TestRecoveryRenderer, test_recovery_renderer, TEST, RECOVERY_RENDERER, GObject)

struct _TestRecoveryRenderer {
  GObject parent_instance;
};

static void recovery_renderer_iface_init (BroMessageRendererInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestRecoveryRenderer, test_recovery_renderer, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MESSAGE_RENDERER, recovery_renderer_iface_init))

static char *
recovery_render (BroMessageRenderer *r, const BroBroMessage *message)
{
  GString *text = g_string_new (bro_message_code_wire (message->code));
  for (guint i = 0; i < message->params->keys->len; i++)
    {
      BroJsonValue *v = message->params->items->pdata[i];
      g_string_append_printf (text, " %s=", (const char *) message->params->keys->pdata[i]);
      if (v->kind == BRO_JSON_VALUE_STRING)
        g_string_append (text, v->string);
      else
        {
          g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (v);
          g_string_append_len (text, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
        }
    }
  return g_string_free (text, FALSE);
}

static void recovery_renderer_iface_init (BroMessageRendererInterface *iface) { iface->render = recovery_render; }
static void test_recovery_renderer_class_init (TestRecoveryRendererClass *k) {}
static void test_recovery_renderer_init (TestRecoveryRenderer *r) {}

/* ---- the harness: the library harness's (executor, clock, tree, database, settle, results), plus the rest ---- */

typedef struct {
  LibraryHarness lib; /* first: the library harness's helpers take it */
  TestMemoryStore *store;
  TestRecoveryIsolation *isolation;
  TestRecoveryRenderer *renderer;
  BroStepRegistry *registry;
  BroLibraryService *library;
  BroRecoveryService *recovery;
  int recover_index; /* -1: none started */
} RecoveryHarness;

static void
recovery_event_seen (BroEngineEvent *e, gpointer data)
{
  g_ptr_array_add (data, bro_engine_event_ref (e));
}

static void
recovery_build_action (gpointer data)
{
  RecoveryHarness *h = data;
  static const BroEngineEventType types[] = { BRO_ENGINE_EVENT_TYPE_JOB_CHANGED, BRO_ENGINE_EVENT_TYPE_JOB_FINISHED,
                                              BRO_ENGINE_EVENT_TYPE_PROBLEM_RAISED };
  g_autoptr (BroSchedulerLimits) limits = bro_scheduler_limits_new ();
  h->lib.bus = bro_event_bus_new (BRO_CLOCK (h->lib.clock), 1, 1024, 1024);
  h->lib.problems = bro_problem_service_new (h->lib.bus, BRO_CLOCK (h->lib.clock));
  h->lib.scheduler = bro_scheduler_new (h->lib.executor, h->lib.bus, limits);
  h->library = bro_library_service_new (h->lib.executor, h->lib.bus, h->lib.problems, h->lib.scheduler, BRO_CONFIG_SOURCE (h->lib.config),
                                        h->lib.engine_clock, BRO_FILE_SYSTEM (h->lib.tree), BRO_LIBRARY_REPOSITORY (h->lib.db),
                                        BRO_UNIT_REPOSITORY (h->lib.db), NULL, NULL);
  h->recovery = bro_recovery_service_new (h->lib.executor, h->lib.bus, h->lib.problems, BRO_STORE (h->store), h->registry, h->library,
                                          BRO_CONFIG_SOURCE (h->lib.config), h->lib.engine_clock, BRO_FILE_SYSTEM (h->lib.tree),
                                          BRO_SETTINGS_ISOLATION (h->isolation), BRO_MESSAGE_RENDERER (h->renderer), "/data");
  h->lib.subscription = bro_event_bus_subscribe (h->lib.bus, types, G_N_ELEMENTS (types), recovery_event_seen, h->lib.events, NULL);
}

static void
recovered_cb (BroBroMessage *summary, const BroBroError *error, gpointer data)
{
  RecoveryHarness *h = data;
  library_record (&h->lib, (guint) h->recover_index,
                  error ? library_error_json (error) : summary ? bro_bro_message_to_json (summary) : bro_json_value_new_null ());
  bro_bro_message_free (summary);
}

static void
recover_action (gpointer data)
{
  RecoveryHarness *h = data;
  bro_recovery_service_recover (h->recovery, recovered_cb, h, NULL);
}

typedef struct {
  RecoveryHarness *h;
  guint index;
} RecoveryOp;

static void
recovered_action (gpointer data)
{
  RecoveryOp *o = data;
  RecoveryHarness *h = o->h;
  gboolean done = h->recover_index >= 0 && h->lib.done[h->recover_index];
  library_record (&h->lib, o->index, bro_json_value_new_bool (done));
}

/* 'jobChanged <id> <state>', 'jobFinished <id> <outcome>[ <error code>]', 'problemRaised <code>'. */
static void
recovery_gather_action (gpointer data)
{
  RecoveryOp *o = data;
  RecoveryHarness *h = o->h;
  BroJsonValue *r = bro_json_value_new_object ();
  BroJsonValue *lines = bro_json_value_new_array ();
  for (guint i = 0; i < h->lib.events->len; i++)
    {
      BroEngineEvent *e = h->lib.events->pdata[i];
      BroJsonValue *job = bro_json_value_member (e->data, "job");
      g_autofree char *line = NULL;
      if (e->type == BRO_ENGINE_EVENT_TYPE_JOB_CHANGED)
        line = g_strdup_printf ("jobChanged %s %s", bro_json_value_get_string (bro_json_value_member (job, "id"), ""),
                                bro_json_value_get_string (bro_json_value_member (job, "state"), ""));
      else if (e->type == BRO_ENGINE_EVENT_TYPE_JOB_FINISHED)
        {
          const char *code = bro_json_value_get_string (bro_json_value_member (bro_json_value_member (job, "error"), "code"), NULL);
          line = g_strdup_printf ("jobFinished %s %s%s%s", bro_json_value_get_string (bro_json_value_member (job, "id"), ""),
                                  bro_json_value_get_string (bro_json_value_member (job, "outcome"), ""), code ? " " : "", code ? code : "");
        }
      else
        line = g_strdup_printf ("problemRaised %s",
                                bro_json_value_get_string (bro_json_value_member (bro_json_value_member (e->data, "problem"), "code"), ""));
      bro_json_value_append (lines, bro_json_value_new_string (line));
    }
  bro_json_value_set (r, "lines", lines);
  bro_json_value_set (r, "problems", problems_json (h->lib.problems));
  library_record (&h->lib, o->index, r);
}

static BroJsonValue *
recovery_error_json (const BroBroError *e)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "code", bro_json_value_new_string (e->code));
  if (bro_json_value_length (e->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (e->params));
  return o;
}

static BroJsonValue *
recovery_projection (const BroJobRecord *r)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "state", bro_json_value_new_string (bro_job_state_to_wire (r->state)));
  bro_json_value_set (o, "queue", bro_json_value_new_string (bro_queue_to_wire (r->queue)));
  if (r->plan)
    {
      BroJsonValue *plan = bro_json_value_new_array ();
      for (guint i = 0; i < r->plan->steps->len; i++)
        bro_json_value_append (plan, bro_json_value_new_string (bro_step_kind_to_wire (g_array_index (r->plan->steps, BroStepKind, i))));
      bro_json_value_set (o, "plan", plan);
    }
  else
    bro_json_value_set (o, "plan", bro_json_value_new_null ());
  bro_json_value_set (o, "mediaGeneration", r->has_media_generation ? bro_json_value_new_integer (r->media_generation) : bro_json_value_new_null ());
  if (r->has_outcome)
    bro_json_value_set (o, "outcome", bro_json_value_new_string (bro_outcome_to_wire (r->outcome)));
  if (r->error)
    bro_json_value_set (o, "error", recovery_error_json (r->error));
  if (r->decision)
    bro_json_value_set (o, "decision", bro_json_value_ref (r->decision));
  return o;
}

static BroBroError *
recovery_case_error (BroJsonValue *j)
{
  BroJsonValue *params = bro_json_value_member (j, "params");
  return bro_bro_error_new (bro_json_value_get_string (bro_json_value_member (j, "code"), ""), params ? bro_json_value_ref (params) : NULL, NULL);
}

static void
recovery_given_jobs (RecoveryHarness *h, BroJsonValue *jobs)
{
  for (guint i = 0; jobs && i < bro_json_value_length (jobs); i++)
    {
      BroJsonValue *j = bro_json_value_at (jobs, i);
      BroJsonValue *plan = bro_json_value_member (j, "plan");
      BroJsonValue *rows = bro_json_value_member (j, "rows");
      BroJsonValue *v;
      g_autoptr (BroJobRecord) r = bro_job_record_new ();
      g_strlcpy (r->id.value, bro_json_value_get_string (bro_json_value_member (j, "id"), ""), sizeof r->id.value);
      r->kind = BRO_JOB_KIND_VIDEO_DISC;
      g_assert_true (bro_job_state_from_wire (bro_json_value_get_string (bro_json_value_member (j, "state"), ""), &r->state));
      if ((v = bro_json_value_member (j, "outcome")))
        {
          r->has_outcome = TRUE;
          g_assert_true (bro_outcome_from_wire (bro_json_value_get_string (v, ""), &r->outcome));
        }
      if ((v = bro_json_value_member (j, "error")))
        r->error = recovery_case_error (v);
      if ((v = bro_json_value_member (j, "decision")))
        r->decision = bro_json_value_ref (v);
      g_assert_true (bro_queue_from_wire (bro_json_value_get_string (bro_json_value_member (j, "queue"), "acquisition"), &r->queue));
      r->title = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "title"), ""));
      r->drive_id = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "driveId"), NULL));
      r->automatic = bro_json_value_get_bool (bro_json_value_member (j, "automatic"), FALSE);
      if ((v = bro_json_value_member (j, "mediaGeneration")))
        {
          r->has_media_generation = TRUE;
          r->media_generation = bro_json_value_get_integer (v, 0);
        }
      r->request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, r->title);
      r->request->automatic = r->automatic;
      r->request->drive_id = g_strdup (r->drive_id);
      r->request->has_media_generation = r->has_media_generation;
      r->request->media_generation = r->media_generation;
      if (plan && plan->kind == BRO_JSON_VALUE_ARRAY)
        {
          r->plan = bro_job_plan_new ();
          for (guint k = 0; k < bro_json_value_length (plan); k++)
            {
              BroStepKind sk;
              g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_at (plan, k), ""), &sk));
              g_array_append_val (r->plan->steps, sk);
            }
        }
      if ((v = bro_json_value_member (j, "unitId")))
        {
          r->has_unit_id = TRUE;
          g_strlcpy (r->unit_id.value, bro_json_value_get_string (v, ""), sizeof r->unit_id.value);
        }
      r->created_at = bro_clock_now (BRO_CLOCK (h->lib.clock));
      bro_job_repository_insert (BRO_JOB_REPOSITORY (h->store), r, NULL);
      for (guint k = 0; k < bro_json_value_length (rows); k++)
        {
          BroJsonValue *row = bro_json_value_at (rows, k);
          g_autoptr (BroStepRecord) s = bro_step_record_new ();
          s->job_id = r->id;
          s->seq = (int) k;
          g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_member (row, "kind"), ""), &s->kind));
          g_assert_true (bro_step_state_from_wire (bro_json_value_get_string (bro_json_value_member (row, "state"), ""), &s->state));
          s->attempt = 1;
          s->has_started_at = TRUE;
          s->started_at = r->created_at;
          s->output = bro_step_output_new (s->kind, bro_json_value_new_object (), NULL);
          s->output->read_errors = (int) bro_json_value_get_integer (bro_json_value_member (row, "readErrors"), 0);
          if ((v = bro_json_value_member (row, "error")))
            s->error = recovery_case_error (v);
          if ((v = bro_json_value_member (row, "checkpoint")))
            s->checkpoint = bro_json_value_ref (v);
          bro_step_repository_save (BRO_STEP_REPOSITORY (h->store), s, NULL);
        }
    }
}

static void
recovery_given_units (RecoveryHarness *h, BroJsonValue *given)
{
  BroJsonValue *list = bro_json_value_member (given, "units");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *u = bro_json_value_at (list, i);
      BroUnitRecord *unit = bro_unit_record_new ();
      g_strlcpy (unit->id.value, bro_json_value_get_string (bro_json_value_member (u, "id"), ""), sizeof unit->id.value);
      unit->library_id = g_strdup (bro_json_value_get_string (bro_json_value_member (u, "library"), ""));
      unit->has_job_id = TRUE;
      g_strlcpy (unit->job_id.value, bro_json_value_get_string (bro_json_value_member (u, "job"), ""), sizeof unit->job_id.value);
      unit->path = g_strdup (bro_json_value_get_string (bro_json_value_member (u, "path"), ""));
      unit->record_file = g_strdup ("bromelia-00000000.json");
      unit->record_version = 3;
      unit->state = BRO_UNIT_STATE_COMMITTING;
      unit->status = BRO_UNIT_STATUS_SUCCESS;
      unit->name = g_strdup (bro_json_value_get_string (bro_json_value_member (u, "name"), ""));
      unit->kind = BRO_MEDIA_KIND_MOVIE;
      unit->format = g_strdup ("bluray");
      unit->format_code = g_strdup ("BD");
      unit->label = g_strdup ("");
      unit->makemkv_version = g_strdup ("");
      unit->attempts = 1;
      unit->created_at = bro_clock_now (BRO_CLOCK (h->lib.clock));
      g_hash_table_replace (h->lib.db->units, g_strdup (unit->id.value), unit);
    }
  list = bro_json_value_member (given, "intents");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *x = bro_json_value_at (list, i);
      BroJsonValue *items = bro_json_value_member (x, "items");
      BroCommitIntent *intent = bro_commit_intent_new ();
      g_strlcpy (intent->unit_id.value, bro_json_value_get_string (bro_json_value_member (x, "unit"), ""), sizeof intent->unit_id.value);
      g_strlcpy (intent->job_id.value, bro_json_value_get_string (bro_json_value_member (x, "job"), ""), sizeof intent->job_id.value);
      intent->staging = g_strdup (bro_json_value_get_string (bro_json_value_member (x, "staging"), ""));
      intent->destination = g_strdup (bro_json_value_get_string (bro_json_value_member (x, "destination"), ""));
      intent->merge = bro_json_value_get_bool (bro_json_value_member (x, "merge"), FALSE);
      intent->created_at = bro_clock_now (BRO_CLOCK (h->lib.clock));
      for (guint k = 0; k < bro_json_value_length (items); k++)
        {
          BroJsonValue *it = bro_json_value_at (items, k);
          BroCommitItem *item = bro_commit_item_new ();
          item->seq = (int) bro_json_value_get_integer (bro_json_value_at (it, 0), 0);
          item->from_path = g_strdup (bro_json_value_get_string (bro_json_value_at (it, 1), ""));
          item->to_path = g_strdup (bro_json_value_get_string (bro_json_value_at (it, 2), ""));
          item->sha256 = g_strdup (bro_json_value_get_string (bro_json_value_at (it, 3), NULL));
          item->is_dir = bro_json_value_get_bool (bro_json_value_at (it, 4), FALSE);
          item->moved = bro_json_value_get_bool (bro_json_value_at (it, 5), FALSE);
          g_ptr_array_add (intent->items, item);
        }
      g_hash_table_replace (h->lib.db->intents, g_strdup (intent->unit_id.value), intent);
    }
}

static BroJsonValue *
recovery_log_json (TestTree *t, const char *job)
{
  g_autofree char *path = g_strconcat ("/data/jobs/", job, "/" BRO_JOB_LOG_FILE_NAME, NULL);
  GBytes *bytes = g_hash_table_lookup (t->files, path);
  BroJsonValue *a = bro_json_value_new_array ();
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  if (!bytes)
    return a;
  text = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_autoptr (BroJsonValue) o = NULL;
      BroJsonValue *m;
      BroJsonValue *params;
      if (!*lines[i])
        continue;
      g_assert_null (strstr (lines[i], "\"seq\""));
      o = bro_json_value_parse (lines[i], -1);
      m = bro_json_value_new_object ();
      bro_json_value_set (m, "severity", bro_json_value_ref (bro_json_value_member (o, "severity")));
      bro_json_value_set (m, "code", bro_json_value_ref (bro_json_value_member (o, "code")));
      params = bro_json_value_member (o, "params");
      if (params && bro_json_value_length (params) > 0)
        bro_json_value_set (m, "params", bro_json_value_ref (params));
      bro_json_value_append (a, m);
    }
  return a;
}

static gboolean
recovery_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  RecoveryHarness h = { { 0 } };
  BroJsonValue *ops = bro_json_value_member (given, "ops");
  BroJsonValue *list;
  BroJsonValue *want;
  g_autoptr (GPtrArray) steps = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (BroJsonValue) results = bro_json_value_new_array ();
  g_autoptr (BroJsonValue) gathered = NULL;
  RecoveryOp gather;
  BroJsonValue *resumability = bro_json_value_member (given, "resumability");
  library_case_id = id;
  h.recover_index = -1;
  g_mutex_init (&h.lib.lock);
  g_cond_init (&h.lib.cond);
  h.lib.clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  h.lib.executor = bro_engine_executor_new ("recovery-test");
  h.lib.engine_clock = bro_engine_clock_new (BRO_CLOCK (h.lib.clock), h.lib.executor);
  h.lib.tree = g_object_new (TEST_TYPE_TREE, NULL);
  h.lib.db = g_object_new (TEST_TYPE_LIBRARY_DB, NULL);
  h.lib.config = g_object_new (TEST_TYPE_CASE_CONFIG, NULL);
  h.lib.events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  h.lib.n_ops = bro_json_value_length (ops);
  h.lib.results = g_new0 (BroJsonValue *, h.lib.n_ops + 1);
  h.lib.done = g_new0 (gboolean, h.lib.n_ops + 1);
  h.store = g_object_new (TEST_TYPE_MEMORY_STORE, NULL);
  h.store->units = g_object_ref (BRO_UNIT_REPOSITORY (h.lib.db));
  h.store->libraries = g_object_ref (BRO_LIBRARY_REPOSITORY (h.lib.db));
  h.isolation = g_object_new (TEST_TYPE_RECOVERY_ISOLATION, NULL);
  h.renderer = g_object_new (TEST_TYPE_RECOVERY_RENDERER, NULL);
  for (int k = BRO_STEP_KIND_AWAIT_MEDIA; k <= BRO_STEP_KIND_TRANSCODE; k++)
    {
      TestRecoveryStep *s = g_object_new (TEST_TYPE_RECOVERY_STEP, NULL);
      const char *r = bro_json_value_get_string (bro_json_value_member (resumability, bro_step_kind_to_wire ((BroStepKind) k)), NULL);
      s->kind = (BroStepKind) k;
      s->resumability = BRO_RESUMABILITY_IDEMPOTENT;
      if (r)
        g_assert_true (bro_resumability_from_wire (r, &s->resumability));
      g_ptr_array_add (steps, s);
    }
  h.registry = bro_step_registry_new (steps);
  bro_json_value_set (h.lib.config->document, "version", bro_json_value_new_integer (3));
  list = bro_json_value_member (given, "config");
  bro_json_value_set (h.lib.config->document, "libraries", list ? bro_json_value_ref (list) : bro_json_value_new_array ());
  list = bro_json_value_member (given, "dirs");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    tree_make_dirs (h.lib.tree, bro_json_value_get_string (bro_json_value_at (list, i), ""));
  list = bro_json_value_member (given, "files");
  for (guint i = 0; list && i < list->keys->len; i++)
    tree_put (h.lib.tree, list->keys->pdata[i], bro_json_value_get_string (list->items->pdata[i], ""));
  list = bro_json_value_member (given, "hold");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *x = bro_json_value_at (list, i);
      g_ptr_array_add (h.lib.tree->holds, g_strconcat (bro_json_value_get_string (bro_json_value_member (x, "op"), ""), " ",
                                                       bro_json_value_get_string (bro_json_value_member (x, "path"), ""), NULL));
    }
  list = bro_json_value_member (given, "libraries");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *l = bro_json_value_at (list, i);
      BroLibraryRecord *r = bro_library_record_new ();
      const char *seen = bro_json_value_get_string (bro_json_value_member (l, "lastSeenAt"), NULL);
      r->id = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "id"), ""));
      r->name = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "name"), ""));
      r->path = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "path"), ""));
      r->marker_id = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "markerId"), ""));
      bro_instant_parse (bro_json_value_get_string (bro_json_value_member (l, "attachedAt"), ""), &r->attached_at);
      r->online = bro_json_value_get_bool (bro_json_value_member (l, "online"), FALSE);
      r->has_last_seen_at = seen && bro_instant_parse (seen, &r->last_seen_at);
      g_hash_table_replace (h.lib.db->libraries, g_strdup (r->id), r);
    }
  recovery_given_units (&h, given);
  recovery_given_jobs (&h, bro_json_value_member (given, "jobs"));
  list = bro_json_value_member (given, "isolationProblems");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    g_ptr_array_add (h.isolation->problems, bro_bro_message_decode (bro_json_value_at (list, i)));
  bro_engine_executor_post (h.lib.executor, recovery_build_action, &h, NULL);
  library_settle (&h.lib);

  for (guint i = 0; i < h.lib.n_ops; i++)
    {
      BroJsonValue *op = bro_json_value_at (ops, i);
      const char *name = op->kind == BRO_JSON_VALUE_STRING ? op->string : op->keys->pdata[0];
      BroJsonValue *arg = op->kind == BRO_JSON_VALUE_OBJECT ? op->items->pdata[0] : NULL;
      if (strcmp (name, "recover") == 0)
        {
          h.recover_index = (int) i;
          bro_engine_executor_post (h.lib.executor, recover_action, &h, NULL);
          if (!arg || bro_json_value_get_bool (bro_json_value_member (arg, "wait"), TRUE))
            library_wait_for (&h.lib, i);
          library_settle (&h.lib);
        }
      else if (strcmp (name, "recovered") == 0)
        {
          RecoveryOp o = { &h, i };
          library_settle (&h.lib);
          bro_engine_executor_post (h.lib.executor, recovered_action, &o, NULL);
          library_wait_for (&h.lib, i);
        }
      else if (strcmp (name, "advance") == 0)
        {
          manual_advance (h.lib.clock, (gint64) (bro_json_value_get_number (arg, 0) * 1000), library_settle, &h.lib);
          library_settle (&h.lib);
          library_record (&h.lib, i, bro_json_value_new_string ("ok"));
        }
      else if (strcmp (name, "release") == 0)
        {
          gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
          tree_release (h.lib.tree);
          while (tree_held (h.lib.tree) > 0)
            {
              if (g_get_monotonic_time () > until)
                g_error ("%s: the held calls didn't leave", library_case_id);
              g_usleep (1000);
            }
          library_settle (&h.lib);
          library_record (&h.lib, i, bro_json_value_new_string ("ok"));
        }
      else
        g_error ("%s: unknown op %s", id, name);
    }
  if (h.recover_index >= 0)
    library_wait_for (&h.lib, (guint) h.recover_index);
  library_settle (&h.lib);

  for (guint i = 0; i < h.lib.n_ops; i++)
    bro_json_value_append (results, h.lib.results[i] ? bro_json_value_ref (h.lib.results[i]) : bro_json_value_new_null ());
  bro_test_same_json (failures, id, "results", bro_json_value_member (expect, "results"), results);
  if ((want = bro_json_value_member (expect, "jobs")))
    for (guint i = 0; i < want->keys->len; i++)
      {
        BroId jid = { { 0 } };
        g_autoptr (BroJobRecord) r = NULL;
        g_autoptr (BroJsonValue) got = NULL;
        g_autofree char *what = g_strconcat ("job ", (const char *) want->keys->pdata[i], NULL);
        g_strlcpy (jid.value, want->keys->pdata[i], sizeof jid.value);
        r = bro_job_repository_load (BRO_JOB_REPOSITORY (h.store), jid, NULL);
        got = recovery_projection (r);
        bro_test_same_json (failures, id, what, want->items->pdata[i], got);
      }
  if ((want = bro_json_value_member (expect, "steps")))
    for (guint i = 0; i < want->keys->len; i++)
      {
        g_autoptr (GPtrArray) rows = steps_of (h.store, want->keys->pdata[i], FALSE);
        g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
        g_autofree char *what = g_strconcat ("steps of ", (const char *) want->keys->pdata[i], NULL);
        for (guint k = 0; k < rows->len; k++)
          {
            BroStepRecord *s = rows->pdata[k];
            g_autofree char *line = g_strdup_printf ("%s %s%s", bro_step_kind_to_wire (s->kind), bro_step_state_to_wire (s->state),
                                                     s->checkpoint ? " checkpoint" : "");
            bro_json_value_append (got, bro_json_value_new_string (line));
          }
        bro_test_same_json (failures, id, what, want->items->pdata[i], got);
      }
  if ((want = bro_json_value_member (expect, "logs")))
    for (guint i = 0; i < want->keys->len; i++)
      {
        g_autoptr (BroJsonValue) got = recovery_log_json (h.lib.tree, want->keys->pdata[i]);
        g_autofree char *what = g_strconcat ("log of ", (const char *) want->keys->pdata[i], NULL);
        bro_test_same_json (failures, id, what, want->items->pdata[i], got);
      }
  gather.h = &h;
  gather.index = h.lib.n_ops;
  bro_engine_executor_post (h.lib.executor, recovery_gather_action, &gather, NULL);
  library_wait_for (&h.lib, h.lib.n_ops);
  gathered = bro_json_value_ref (h.lib.results[h.lib.n_ops]);
  if ((want = bro_json_value_member (expect, "eventLines")))
    bro_test_same_json (failures, id, "event lines", want, bro_json_value_member (gathered, "lines"));
  if ((want = bro_json_value_member (expect, "problems")))
    bro_test_same_json (failures, id, "problems", want, bro_json_value_member (gathered, "problems"));
  if ((want = bro_json_value_member (expect, "isolation")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      for (guint i = 0; i < h.isolation->log->len; i++)
        bro_json_value_append (got, bro_json_value_new_string (h.isolation->log->pdata[i]));
      bro_test_same_json (failures, id, "isolation", want, got);
    }
  if ((want = bro_json_value_member (expect, "files")))
    {
      g_autoptr (BroJsonValue) all = tree_files_json (h.lib.tree);
      g_autoptr (BroJsonValue) got = bro_json_value_new_object ();
      for (guint i = 0; i < all->keys->len; i++)
        {
          const char *path = all->keys->pdata[i];
          if (g_str_has_prefix (path, "/data/jobs/") && g_str_has_suffix (path, "/" BRO_JOB_LOG_FILE_NAME))
            continue;
          bro_json_value_set (got, path, bro_json_value_ref (all->items->pdata[i]));
        }
      same_sorted (failures, id, "files", want, got);
    }
  if ((want = bro_json_value_member (expect, "dirsUnder")))
    for (guint i = 0; i < want->keys->len; i++)
      {
        g_autoptr (BroJsonValue) dirs = sorted_set_json (h.lib.tree->dirs);
        g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
        g_autofree char *prefix = g_strconcat (want->keys->pdata[i], "/", NULL);
        for (guint k = 0; k < bro_json_value_length (dirs); k++)
          if (g_str_has_prefix (bro_json_value_get_string (bro_json_value_at (dirs, k), ""), prefix))
            bro_json_value_append (got, bro_json_value_ref (bro_json_value_at (dirs, k)));
        bro_test_same_json (failures, id, "folders under", want->items->pdata[i], got);
      }
  if ((want = bro_json_value_member (expect, "db")))
    {
      BroJsonValue *part;
      if ((part = bro_json_value_member (want, "units")))
        {
          g_autoptr (BroJsonValue) units = db_units_json (h.lib.db);
          g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
          for (guint i = 0; i < bro_json_value_length (units); i++)
            {
              BroJsonValue *u = bro_json_value_at (units, i);
              BroJsonValue *o = bro_json_value_new_object ();
              bro_json_value_set (o, "id", bro_json_value_ref (bro_json_value_member (u, "id")));
              bro_json_value_set (o, "state", bro_json_value_ref (bro_json_value_member (u, "state")));
              bro_json_value_append (got, o);
            }
          bro_test_same_json (failures, id, "db units", part, got);
        }
      if ((part = bro_json_value_member (want, "intents")))
        {
          g_autoptr (GPtrArray) open = db_open_intents (BRO_UNIT_REPOSITORY (h.lib.db), NULL);
          g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
          for (guint i = 0; i < open->len; i++)
            bro_json_value_append (got, bro_json_value_new_string (((BroCommitIntent *) open->pdata[i])->unit_id.value));
          bro_test_same_json (failures, id, "db intents", part, got);
        }
    }

  if (h.lib.subscription)
    bro_subscription_cancel (h.lib.subscription);
  bro_engine_executor_stop (h.lib.executor);
  g_clear_object (&h.recovery);
  g_clear_object (&h.library);
  g_clear_object (&h.lib.subscription);
  g_clear_object (&h.lib.scheduler);
  g_clear_object (&h.lib.problems);
  g_clear_object (&h.lib.bus);
  g_object_unref (h.registry);
  g_object_unref (h.isolation);
  g_object_unref (h.renderer);
  g_object_unref (h.store);
  g_object_unref (h.lib.engine_clock);
  g_object_unref (h.lib.executor);
  g_object_unref (h.lib.clock);
  g_object_unref (h.lib.tree);
  g_object_unref (h.lib.db);
  g_object_unref (h.lib.config);
  g_ptr_array_unref (h.lib.events);
  for (guint i = 0; i <= h.lib.n_ops; i++)
    if (h.lib.results[i])
      bro_json_value_unref (h.lib.results[i]);
  g_free (h.lib.results);
  g_free (h.lib.done);
  g_mutex_clear (&h.lib.lock);
  g_cond_clear (&h.lib.cond);
  return TRUE;
}

static void
test_recovery_service (void)
{
  bro_test_run_cases ("engine/recovery-service.cases.json", recovery_case);
}
