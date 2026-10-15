/* test-pipeline.inc: shared/fixtures/engine/pipeline-runner.cases.json and job-planner.cases.json (included by
 * test-engine.c, after test-scheduler.inc). */
#include "bro-job-log.h"
#include "bro-job-planner.h"
#include "bro-pipeline-runner.h"

/* ---- jobs and steps in memory ---------------------------------------------------------------------------------- */

#define TEST_TYPE_MEMORY_STORE (test_memory_store_get_type ())
G_DECLARE_FINAL_TYPE (TestMemoryStore, test_memory_store, TEST, MEMORY_STORE, GObject)

struct _TestMemoryStore {
  GObject parent_instance;
  GMutex lock;
  GHashTable *jobs;  /* id → BroJobRecord */
  GPtrArray *order;  /* job ids in the order they were inserted */
  GHashTable *steps; /* "id/seq" → BroStepRecord */
  BroUnitRepository *units;         /* nullable: a test that needs units sets it */
  BroLibraryRepository *libraries;  /* nullable */
};

static void store_iface_init (BroStoreInterface *iface);
static void store_jobs_iface_init (BroJobRepositoryInterface *iface);
static void store_steps_iface_init (BroStepRepositoryInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestMemoryStore, test_memory_store, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_STORE, store_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_JOB_REPOSITORY, store_jobs_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP_REPOSITORY, store_steps_iface_init))

static BroJobRepository *store_jobs (BroStore *s) { return BRO_JOB_REPOSITORY (s); }
static BroStepRepository *store_steps (BroStore *s) { return BRO_STEP_REPOSITORY (s); }
static BroUnitRepository *store_units (BroStore *s) { return TEST_MEMORY_STORE (s)->units; }
static BroLibraryRepository *store_libraries (BroStore *s) { return TEST_MEMORY_STORE (s)->libraries; }

static gboolean
store_transaction (BroStore *s, BroStoreBlockFunc block, gpointer data, BroBroError **error)
{
  return block (s, data, error);
}

static gboolean
store_migrate (BroStore *s, BroBroError **error)
{
  return TRUE;
}

static void
store_iface_init (BroStoreInterface *iface)
{
  iface->jobs = store_jobs;
  iface->steps = store_steps;
  iface->units = store_units;
  iface->libraries = store_libraries;
  iface->transaction = store_transaction;
  iface->migrate = store_migrate;
}

static gboolean
job_put (BroJobRepository *repo, const BroJobRecord *job, BroBroError **error)
{
  TestMemoryStore *self = TEST_MEMORY_STORE (repo);
  g_mutex_lock (&self->lock);
  if (!g_hash_table_contains (self->jobs, job->id.value))
    g_ptr_array_add (self->order, g_strdup (job->id.value));
  g_hash_table_replace (self->jobs, g_strdup (job->id.value), bro_job_record_copy (job));
  g_mutex_unlock (&self->lock);
  return TRUE;
}

/* Unfinished jobs, in the order they were inserted. */
static GPtrArray *
job_active (BroJobRepository *repo, BroBroError **error)
{
  TestMemoryStore *self = TEST_MEMORY_STORE (repo);
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_job_record_free);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->order->len; i++)
    {
      BroJobRecord *r = g_hash_table_lookup (self->jobs, self->order->pdata[i]);
      if (r->state != BRO_JOB_STATE_FINISHED)
        g_ptr_array_add (found, bro_job_record_copy (r));
    }
  g_mutex_unlock (&self->lock);
  return found;
}

static BroJobRecord *
job_load (BroJobRepository *repo, BroId job_id, BroBroError **error)
{
  TestMemoryStore *self = TEST_MEMORY_STORE (repo);
  BroJobRecord *r;
  g_mutex_lock (&self->lock);
  r = bro_job_record_copy (g_hash_table_lookup (self->jobs, job_id.value));
  g_mutex_unlock (&self->lock);
  return r;
}

static void
store_jobs_iface_init (BroJobRepositoryInterface *iface)
{
  iface->insert = job_put;
  iface->update = job_put;
  iface->load = job_load;
  iface->active = job_active;
}

static gboolean
step_save (BroStepRepository *repo, const BroStepRecord *step, BroBroError **error)
{
  TestMemoryStore *self = TEST_MEMORY_STORE (repo);
  g_mutex_lock (&self->lock);
  g_hash_table_replace (self->steps, g_strdup_printf ("%s/%06d", step->job_id.value, step->seq), bro_step_record_copy (step));
  g_mutex_unlock (&self->lock);
  return TRUE;
}

static int
by_seq (gconstpointer a, gconstpointer b)
{
  return (*(BroStepRecord *const *) a)->seq - (*(BroStepRecord *const *) b)->seq;
}

/* Every step of the job (only the succeeded ones when @succeeded), by seq. */
static GPtrArray *
steps_of (TestMemoryStore *self, const char *job, gboolean succeeded)
{
  GPtrArray *found = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_record_free);
  GHashTableIter it;
  gpointer value;
  g_mutex_lock (&self->lock);
  g_hash_table_iter_init (&it, self->steps);
  while (g_hash_table_iter_next (&it, NULL, &value))
    {
      BroStepRecord *s = value;
      if (strcmp (s->job_id.value, job) == 0 && (!succeeded || s->state == BRO_STEP_STATE_SUCCEEDED))
        g_ptr_array_add (found, bro_step_record_copy (s));
    }
  g_mutex_unlock (&self->lock);
  g_ptr_array_sort (found, by_seq);
  return found;
}

static GPtrArray *
step_completed (BroStepRepository *repo, BroId job_id, BroBroError **error)
{
  return steps_of (TEST_MEMORY_STORE (repo), job_id.value, TRUE);
}

static GPtrArray *
step_load (BroStepRepository *repo, BroId job_id, BroBroError **error)
{
  return steps_of (TEST_MEMORY_STORE (repo), job_id.value, FALSE);
}

static void
store_steps_iface_init (BroStepRepositoryInterface *iface)
{
  iface->save = step_save;
  iface->completed = step_completed;
  iface->load = step_load;
}

static void
test_memory_store_finalize (GObject *object)
{
  TestMemoryStore *self = TEST_MEMORY_STORE (object);
  g_hash_table_unref (self->jobs);
  g_ptr_array_unref (self->order);
  g_hash_table_unref (self->steps);
  g_clear_object (&self->units);
  g_clear_object (&self->libraries);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_memory_store_parent_class)->finalize (object);
}

static void
test_memory_store_class_init (TestMemoryStoreClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_memory_store_finalize;
}

static void
test_memory_store_init (TestMemoryStore *self)
{
  g_mutex_init (&self->lock);
  self->jobs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_job_record_free);
  self->order = g_ptr_array_new_with_free_func (g_free);
  self->steps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_step_record_free);
}

/* ---- files and folders in memory ------------------------------------------------------------------------------- */

#define TEST_TYPE_MEMORY_FS (test_memory_fs_get_type ())
G_DECLARE_FINAL_TYPE (TestMemoryFs, test_memory_fs, TEST, MEMORY_FS, GObject)

struct _TestMemoryFs {
  GObject parent_instance;
  GMutex lock;
  GHashTable *files;   /* path → GBytes */
  GHashTable *folders; /* paths */
};

static void memory_fs_iface_init (BroFileSystemInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestMemoryFs, test_memory_fs, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, memory_fs_iface_init))

static gboolean
fs_exists (BroFileSystem *fs, const char *path)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  gboolean found;
  g_mutex_lock (&self->lock);
  found = g_hash_table_contains (self->files, path) || g_hash_table_contains (self->folders, path);
  g_mutex_unlock (&self->lock);
  return found;
}

static GBytes *
fs_read (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  GBytes *b;
  g_mutex_lock (&self->lock);
  b = g_hash_table_lookup (self->files, path);
  b = b ? g_bytes_ref (b) : NULL;
  g_mutex_unlock (&self->lock);
  if (!b)
    bro_bro_error_set (error, "fs.notFound", NULL);
  return b;
}

static gboolean
fs_create_directory (BroFileSystem *fs, const char *path, gboolean parents_must_exist, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_auto (GStrv) parts = g_strsplit (path, "/", -1);
  GString *current = g_string_new ("");
  g_mutex_lock (&self->lock);
  for (guint i = 0; parts[i]; i++)
    if (*parts[i])
      {
        g_string_append_printf (current, "/%s", parts[i]);
        g_hash_table_add (self->folders, g_strdup (current->str));
      }
  g_mutex_unlock (&self->lock);
  g_string_free (current, TRUE);
  return TRUE;
}

static gboolean
fs_write_atomically (BroFileSystem *fs, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_autofree char *folder = g_path_get_dirname (path);
  gboolean ok;
  g_mutex_lock (&self->lock);
  ok = g_hash_table_contains (self->folders, folder);
  if (ok)
    g_hash_table_replace (self->files, g_strdup (path), g_bytes_ref (bytes));
  g_mutex_unlock (&self->lock);
  if (!ok)
    bro_bro_error_set (error, "fs.notFound", NULL);
  return ok;
}

static void
memory_fs_iface_init (BroFileSystemInterface *iface)
{
  iface->exists = fs_exists;
  iface->read = fs_read;
  iface->create_directory = fs_create_directory;
  iface->write_atomically = fs_write_atomically;
}

static void
test_memory_fs_finalize (GObject *object)
{
  TestMemoryFs *self = TEST_MEMORY_FS (object);
  g_hash_table_unref (self->files);
  g_hash_table_unref (self->folders);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_memory_fs_parent_class)->finalize (object);
}

static void
test_memory_fs_class_init (TestMemoryFsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_memory_fs_finalize;
}

static void
test_memory_fs_init (TestMemoryFs *self)
{
  g_mutex_init (&self->lock);
  self->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
  self->folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add (self->folders, g_strdup ("/"));
}

/* ---- gates a fake step can wait at -------------------------------------------------------------------------- */

typedef struct {
  GMutex lock;
  GCond cond;
  GHashTable *opened;    /* gate names */
  GPtrArray *waiters;    /* "gate\njob id" */
  GHashTable *cancelled; /* job ids */
} Gates;

static void
gates_init (Gates *g)
{
  g_mutex_init (&g->lock);
  g_cond_init (&g->cond);
  g->opened = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g->waiters = g_ptr_array_new_with_free_func (g_free);
  g->cancelled = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static void
gates_clear (Gates *g)
{
  g_hash_table_unref (g->opened);
  g_ptr_array_unref (g->waiters);
  g_hash_table_unref (g->cancelled);
  g_mutex_clear (&g->lock);
  g_cond_clear (&g->cond);
}

static void
gates_wake (gpointer data, gpointer unused)
{
  Gates *g = data;
  g_mutex_lock (&g->lock);
  g_cond_broadcast (&g->cond);
  g_mutex_unlock (&g->lock);
}

/* TRUE when the gate opened, FALSE when the job was cancelled first. */
static gboolean
gates_wait (Gates *g, const char *gate, const BroId *job, BroCancellationToken *cancel)
{
  char *entry = g_strconcat (gate, "\n", job->value, NULL);
  guint id = bro_cancellation_token_on_cancel (cancel, gates_wake, g, NULL);
  gboolean opened;
  g_mutex_lock (&g->lock);
  g_ptr_array_add (g->waiters, entry);
  while (!(opened = g_hash_table_contains (g->opened, gate)) && !bro_cancellation_token_is_cancelled (cancel))
    g_cond_wait (&g->cond, &g->lock);
  g_ptr_array_remove (g->waiters, entry);
  g_mutex_unlock (&g->lock);
  bro_cancellation_token_disconnect (cancel, id);
  return opened;
}

static void
gates_open (Gates *g, const char *gate)
{
  g_mutex_lock (&g->lock);
  g_hash_table_add (g->opened, g_strdup (gate));
  g_cond_broadcast (&g->cond);
  g_mutex_unlock (&g->lock);
}

/* The steps parked at closed gates (not cancelled), and whether @job is one of them. */
static int
gates_parked (Gates *g, const char *job, gboolean *job_there)
{
  int n = 0;
  if (job_there)
    *job_there = FALSE;
  g_mutex_lock (&g->lock);
  for (guint i = 0; i < g->waiters->len; i++)
    {
      const char *entry = g_ptr_array_index (g->waiters, i);
      const char *id = strchr (entry, '\n') + 1;
      g_autofree char *gate = g_strndup (entry, id - 1 - entry);
      if (g_hash_table_contains (g->opened, gate) || g_hash_table_contains (g->cancelled, id))
        continue;
      n++;
      if (job_there && job && strcmp (id, job) == 0)
        *job_there = TRUE;
    }
  g_mutex_unlock (&g->lock);
  return n;
}

/* ---- a step that does what the case says --------------------------------------------------------------------- */

#define TEST_TYPE_FAKE_STEP (test_fake_step_get_type ())
/* ---- the configuration a case sets: given.config, then its config ops (read and changed on the executor) ---- */

#define TEST_TYPE_CASE_CONFIG (test_case_config_get_type ())
G_DECLARE_FINAL_TYPE (TestCaseConfig, test_case_config, TEST, CASE_CONFIG, GObject)

struct _TestCaseConfig {
  GObject parent;
  BroJsonValue *document;
};

static BroConfig *
case_config_current (BroConfigSource *source)
{
  return bro_config_new (bro_json_value_ref (TEST_CASE_CONFIG (source)->document), NULL);
}

static void
case_config_iface_init (BroConfigSourceInterface *iface)
{
  iface->current = case_config_current;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (TestCaseConfig, test_case_config, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CONFIG_SOURCE, case_config_iface_init))

static void
test_case_config_finalize (GObject *object)
{
  bro_json_value_unref (TEST_CASE_CONFIG (object)->document);
  G_OBJECT_CLASS (test_case_config_parent_class)->finalize (object);
}

static void
test_case_config_class_init (TestCaseConfigClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_case_config_finalize;
}

static void
test_case_config_init (TestCaseConfig *self)
{
  self->document = bro_json_value_new_object ();
}

G_DECLARE_FINAL_TYPE (TestFakeStep, test_fake_step, TEST, FAKE_STEP, GObject)

struct _TestFakeStep {
  GObject parent_instance;
  BroStepKind kind;
  BroJsonValue *b; /* behaviour */
  Gates *gates;
};

static void fake_step_iface_init (BroStepInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestFakeStep, test_fake_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, fake_step_iface_init))

static BroStepKind
fake_kind (BroStep *s)
{
  return TEST_FAKE_STEP (s)->kind;
}

static BroResumability
fake_resumability (BroStep *s)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
fake_resources (BroStep *s, const BroJobSnapshot *job)
{
  TestFakeStep *self = TEST_FAKE_STEP (s);
  BroJsonValue *r = bro_json_value_member (self->b, "resources");
  BroResourceSet *set = bro_resource_set_new ();
  g_auto (GStrv) drives = strv_of (r ? bro_json_value_member (r, "drives") : NULL);
  GStrvBuilder *all = g_strv_builder_new ();
  g_strv_builder_addv (all, (const char **) drives);
  if (bro_json_value_get_bool (bro_json_value_member (self->b, "drive"), FALSE) && job->request->drive_id)
    g_strv_builder_add (all, job->request->drive_id);
  set->drives = g_strv_builder_end (all);
  g_strv_builder_unref (all);
  set->acquisition = r && bro_json_value_get_bool (bro_json_value_member (r, "acquisition"), FALSE);
  set->library_writes = strv_of (r ? bro_json_value_member (r, "libraryWrites") : NULL);
  set->cpu = r ? bro_json_value_get_integer (bro_json_value_member (r, "cpu"), 0) : 0;
  set->io = strv_of (r ? bro_json_value_member (r, "io") : NULL);
  return set;
}

static BroBroMessage *
message_of (BroJsonValue *j, BroSeverity severity)
{
  BroMessageCode code = BRO_MSG_INTERNAL_UNEXPECTED;
  BroJsonValue *params = bro_json_value_member (j, "params");
  g_assert_true (bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (j, "code"), ""), &code));
  return bro_bro_message_new (code, params ? bro_json_value_ref (params) : NULL, severity);
}

static BroStepReport *
fake_run (BroStep *s, const BroStepContext *ctx, BroBroError **error)
{
  TestFakeStep *self = TEST_FAKE_STEP (s);
  BroJsonValue *b = self->b, *v;
  BroJsonValue *progress = bro_json_value_member (b, "progress"), *log = bro_json_value_member (b, "log");
  g_autoptr (BroJsonValue) data = NULL;
  BroStepReport *report;
  for (guint i = 0; progress && i < bro_json_value_length (progress); i++)
    {
      BroJsonValue *p = bro_json_value_at (progress, i);
      BroMessageCode code;
      g_autoptr (BroBroMessage) op = NULL;
      g_assert_true (bro_message_code_parse (bro_json_value_get_string (bro_json_value_at (p, 1), ""), &code));
      op = bro_bro_message_new (code, NULL, BRO_SEVERITY_INFO);
      bro_progress_reporter_report (ctx->progress, bro_json_value_get_number (bro_json_value_at (p, 0), 0), op, NULL);
    }
  for (guint i = 0; log && i < bro_json_value_length (log); i++)
    {
      BroMessageCode code;
      g_autoptr (BroBroMessage) m = NULL;
      g_assert_true (bro_message_code_parse (bro_json_value_get_string (bro_json_value_at (log, i), ""), &code));
      m = bro_bro_message_new (code, NULL, BRO_SEVERITY_INFO);
      bro_job_logger_log (ctx->log, m);
    }
  v = bro_json_value_member (b, "output");
  data = v ? bro_json_value_ref (v) : bro_json_value_new_object ();
  if ((v = bro_json_value_member (b, "config")))
    {
      BroJsonValue *member = bro_json_value_member (ctx->config->document, v->string);
      bro_json_value_unref (data);
      data = bro_json_value_new_object ();
      bro_json_value_set (data, v->string, member ? bro_json_value_ref (member) : bro_json_value_new_null ());
    }
  if ((v = bro_json_value_member (b, "echo")))
    for (guint i = ctx->job->outputs->len; i > 0; i--)
      {
        BroStepOutput *o = g_ptr_array_index (ctx->job->outputs, i - 1);
        if (strcmp (bro_step_kind_to_wire (o->kind), v->string) == 0)
          {
            bro_json_value_unref (data);
            data = bro_json_value_ref (o->data);
            break;
          }
      }
  if ((v = bro_json_value_member (b, "ask")))
    {
      g_autoptr (BroDecisionDto) d = bro_decision_dto_new (bro_json_value_get_string (bro_json_value_member (v, "id"), ""),
                                                           message_of (bro_json_value_member (v, "question"), BRO_SEVERITY_INFO));
      BroJsonValue *answers = bro_json_value_member (v, "answers");
      g_autofree char *answer = NULL;
      for (guint i = 0; i < bro_json_value_length (answers); i++)
        {
          const char *a = bro_json_value_get_string (bro_json_value_at (answers, i), "");
          g_autofree char *label = g_strconcat ("decision.answer.", a, NULL);
          BroMessageCode code;
          g_assert_true (bro_message_code_parse (label, &code));
          g_ptr_array_add (d->answers, bro_decision_answer_new (a, bro_bro_message_new (code, NULL, BRO_SEVERITY_INFO)));
        }
      answer = bro_decision_port_ask (ctx->decisions, d, error);
      if (!answer)
        return NULL;
      bro_json_value_unref (data);
      data = bro_json_value_new_object ();
      bro_json_value_set (data, "answer", bro_json_value_new_string (answer));
    }
  if ((v = bro_json_value_member (b, "gate")) && !gates_wait (self->gates, v->string, &ctx->job->id, ctx->cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return NULL;
    }
  report = bro_step_report_new (g_steal_pointer (&data));
  if ((v = bro_json_value_member (b, "append")))
    for (guint i = 0; i < bro_json_value_length (v); i++)
      {
        BroStepKind k;
        g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_at (v, i), ""), &k));
        g_array_append_val (report->append_steps, k);
      }
  report->skipped = bro_json_value_get_bool (bro_json_value_member (b, "skipped"), FALSE);
  report->read_errors = bro_json_value_get_integer (bro_json_value_member (b, "readErrors"), 0);
  report->quarantined = bro_json_value_get_bool (bro_json_value_member (b, "quarantined"), FALSE);
  if ((v = bro_json_value_member (b, "notice")))
    {
      const char *n = v->string;
      BroMakemkvNoticeKind kind = strcmp (n, "keyExpired") == 0 ? BRO_MAKEMKV_NOTICE_KEY_EXPIRED
                                  : strcmp (n, "evaluationNotStarted") == 0 ? BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED
                                  : strcmp (n, "versionTooOld") == 0 ? BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD
                                                                       : BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED;
      report->notice = bro_makemkv_notice_new (kind, NULL);
    }
  if ((v = bro_json_value_member (b, "skipJob")))
    report->skip_job = message_of (v, BRO_SEVERITY_WARNING);
  if ((v = bro_json_value_member (b, "fail")))
    {
      BroJsonValue *params = bro_json_value_member (v, "params");
      report->error = bro_bro_error_new (bro_json_value_get_string (bro_json_value_member (v, "code"), ""),
                                         params ? bro_json_value_ref (params) : NULL, NULL);
    }
  report->affects_outcome = bro_json_value_get_bool (bro_json_value_member (b, "affectsOutcome"), TRUE);
  return report;
}

static void
fake_step_iface_init (BroStepInterface *iface)
{
  iface->kind = fake_kind;
  iface->resumability = fake_resumability;
  iface->resources = fake_resources;
  iface->run = fake_run;
}

static void
test_fake_step_finalize (GObject *object)
{
  bro_json_value_unref (TEST_FAKE_STEP (object)->b);
  G_OBJECT_CLASS (test_fake_step_parent_class)->finalize (object);
}

static void
test_fake_step_class_init (TestFakeStepClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_fake_step_finalize;
}

static void
test_fake_step_init (TestFakeStep *self)
{
}

/* ---- the harness -------------------------------------------------------------------------------------------- */

typedef struct {
  World w;
  Gates gates;
  BroEventBus *bus;
  BroScheduler *scheduler;
  TestMemoryStore *store;
  TestMemoryFs *fs;
  TestCaseConfig *config;
  BroPipelineRunner *runner;
  BroSubscription *subscription;
  GPtrArray *names;     /* job names, in order */
  GHashTable *jobs;     /* name → BroId */
  GHashTable *outcomes; /* name → BroDecided */
  GHashTable *started;  /* names */
  GPtrArray *events;    /* BroEngineEvent */
  GPtrArray *failures;
  const char *id;
} PipeHarness;

static const char *
pipe_name (PipeHarness *h, const char *id)
{
  for (guint i = 0; i < h->names->len; i++)
    if (strcmp (((BroId *) g_hash_table_lookup (h->jobs, g_ptr_array_index (h->names, i)))->value, id) == 0)
      return g_ptr_array_index (h->names, i);
  return "?";
}

typedef struct {
  PipeHarness *h;
  char *name;
} Runner;

static void
runner_free (gpointer data)
{
  Runner *r = data;
  g_free (r->name);
  g_free (r);
}

static void
run_done (const BroDecided *decided, const BroBroError *error, gpointer data)
{
  Runner *r = data;
  if (decided)
    {
      BroDecided *d = g_new0 (BroDecided, 1);
      d->outcome = decided->outcome;
      d->error = bro_bro_error_copy (decided->error);
      g_hash_table_replace (r->h->outcomes, g_strdup (r->name), d);
    }
}

typedef struct {
  PipeHarness *h;
  BroJsonValue *op;
  BroBroError *error;
  gboolean ok;
  GMutex lock;
  GCond cond;
  gboolean done;
} PipeOp;

static void
pipe_op_action (gpointer data)
{
  PipeOp *o = data;
  PipeHarness *h = o->h;
  BroJsonValue *arg;
  const char *name = op_name (o->op, &arg);
  if (strcmp (name, "run") == 0)
    {
      Runner *r = g_new0 (Runner, 1);
      r->h = h;
      r->name = g_strdup (arg->string);
      g_hash_table_add (h->started, g_strdup (arg->string));
      bro_pipeline_runner_run (h->runner, g_hash_table_lookup (h->jobs, arg->string), run_done, r, runner_free);
    }
  else if (strcmp (name, "cancel") == 0)
    bro_pipeline_runner_cancel (h->runner, g_hash_table_lookup (h->jobs, arg->string));
  else if (strcmp (name, "decide") == 0)
    o->ok = bro_pipeline_runner_decide (h->runner, g_hash_table_lookup (h->jobs, bro_json_value_get_string (bro_json_value_member (arg, "job"), "")),
                                        bro_json_value_get_string (bro_json_value_member (arg, "answer"), ""), &o->error);
  else if (strcmp (name, "config") == 0)
    {
      bro_json_value_unref (h->config->document);
      h->config->document = bro_json_value_ref (arg);
    }
  else if (strcmp (name, "library") == 0)
    {
      const char *lib = bro_json_value_get_string (bro_json_value_member (arg, "id"), "");
      if (bro_json_value_get_bool (bro_json_value_member (arg, "online"), TRUE))
        bro_scheduler_set_library_offline (h->scheduler, lib, NULL);
      else
        {
          BroJsonValue *params = bro_json_value_new_object ();
          g_autoptr (BroBroError) reason = NULL;
          bro_json_value_set (params, "library", bro_json_value_new_string (lib));
          reason = bro_bro_error_new (bro_message_code_wire (BRO_MSG_LIBRARY_OFFLINE), params, NULL);
          bro_scheduler_set_library_offline (h->scheduler, lib, reason);
        }
    }
}

static void
pipe_subscribe (gpointer data)
{
  PipeHarness *h = data;
  h->subscription = bro_event_bus_subscribe (h->bus, NULL, 0, event_seen, h->events, NULL);
}

typedef struct {
  PipeHarness *h;
  gboolean settled;
  GMutex lock;
  GCond cond;
  gboolean done;
} Check;

/* On the executor: everything still going on is parked (a step at a closed gate, a step waiting for an answer), and
 * every job that was run has finished or waits (for resources, an answer, a gate). */
static void
pipe_check (gpointer data)
{
  Check *c = data;
  PipeHarness *h = c->h;
  int parked = gates_parked (&h->gates, NULL, NULL);
  gboolean ok = TRUE;
  for (guint i = 0; i < h->names->len; i++)
    {
      const char *name = g_ptr_array_index (h->names, i);
      BroId *id = g_hash_table_lookup (h->jobs, name);
      const char *state = NULL;
      gboolean at_gate;
      if (!g_hash_table_contains (h->started, name) || g_hash_table_contains (h->outcomes, name))
        continue;
      for (guint k = h->events->len; k > 0 && !state; k--)
        {
          BroEngineEvent *e = g_ptr_array_index (h->events, k - 1);
          BroJsonValue *job = bro_json_value_member (e->data, "job");
          if ((e->type == BRO_ENGINE_EVENT_TYPE_JOB_CHANGED || e->type == BRO_ENGINE_EVENT_TYPE_JOB_FINISHED)
              && g_strcmp0 (bro_json_value_get_string (bro_json_value_member (job, "id"), ""), id->value) == 0)
            state = bro_json_value_get_string (bro_json_value_member (job, "state"), "");
        }
      gates_parked (&h->gates, id->value, &at_gate);
      g_mutex_lock (&h->gates.lock);
      if (g_strcmp0 (state, "awaitingDecision") == 0 && !g_hash_table_contains (h->gates.cancelled, id->value))
        parked++;
      else if (!(g_strcmp0 (state, "waitingForResources") == 0 || g_strcmp0 (state, "blocked") == 0 || at_gate))
        ok = FALSE;
      g_mutex_unlock (&h->gates.lock);
    }
  c->settled = ok && bro_engine_executor_activity (h->w.executor) == parked;
  g_mutex_lock (&c->lock);
  c->done = TRUE;
  g_cond_signal (&c->cond);
  g_mutex_unlock (&c->lock);
}

static void
pipe_settle (PipeHarness *h)
{
  gint64 until = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  for (;;)
    {
      Check c = { h };
      g_mutex_init (&c.lock);
      g_cond_init (&c.cond);
      /* Posted alone (no barrier after it), so the executor's activity counts only the engine's own work. */
      bro_engine_executor_post (h->w.executor, pipe_check, &c, NULL);
      g_mutex_lock (&c.lock);
      while (!c.done)
        g_cond_wait (&c.cond, &c.lock);
      g_mutex_unlock (&c.lock);
      g_mutex_clear (&c.lock);
      g_cond_clear (&c.cond);
      if (c.settled)
        return;
      g_assert_true (g_get_monotonic_time () < until);
      g_usleep (1000);
    }
}

static void
pipe_run_op (PipeHarness *h, BroJsonValue *op)
{
  BroJsonValue *arg;
  const char *name = op_name (op, &arg);
  if (strcmp (name, "open") == 0)
    gates_open (&h->gates, arg->string);
  else if (strcmp (name, "advance") == 0)
    manual_advance (h->w.clock, bro_json_value_get_integer (arg, 0), settle, &h->w);
  else
    {
      PipeOp o = { h, op };
      if (strcmp (name, "cancel") == 0)
        {
          g_mutex_lock (&h->gates.lock);
          g_hash_table_add (h->gates.cancelled, g_strdup (((BroId *) g_hash_table_lookup (h->jobs, arg->string))->value));
          g_mutex_unlock (&h->gates.lock);
        }
      bro_engine_executor_post (h->w.executor, pipe_op_action, &o, NULL);
      bro_engine_executor_barrier (h->w.executor);
      if (strcmp (name, "decide") == 0)
        {
          BroJsonValue *want = bro_json_value_member (arg, "expectError");
          bro_test_same_string (h->failures, h->id, "decide's error", want ? want->string : NULL, o.error ? o.error->code : NULL);
          bro_bro_error_free (o.error);
        }
    }
  pipe_settle (h);
}

static GPtrArray *
pipe_lines (PipeHarness *h, const char *who)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < h->events->len; i++)
    {
      BroEngineEvent *e = g_ptr_array_index (h->events, i);
      BroJsonValue *job = bro_json_value_member (e->data, "job"), *v;
      GString *text = NULL;
      const char *of = NULL;
      if (e->type == BRO_ENGINE_EVENT_TYPE_JOB_CHANGED)
        {
          BroJsonValue *progress = bro_json_value_member (job, "progress");
          of = bro_json_value_get_string (bro_json_value_member (job, "id"), "");
          text = g_string_new (NULL);
          g_string_printf (text, "changed %s %s", bro_json_value_get_string (bro_json_value_member (job, "state"), ""),
                           progress ? bro_json_value_get_string (bro_json_value_member (progress, "step"), "-") : "-");
          if (progress && (v = bro_json_value_member (progress, "currentFraction")))
            g_string_append_printf (text, " progress %d", (int) (bro_json_value_get_number (v, 0) * 100 + 0.5));
          if ((v = bro_json_value_member (job, "blockedBy")))
            g_string_append_printf (text, " blockedBy %s", bro_json_value_get_string (bro_json_value_member (v, "code"), ""));
          if ((v = bro_json_value_member (job, "decision")))
            g_string_append_printf (text, " decision %s", bro_json_value_get_string (bro_json_value_member (v, "id"), ""));
        }
      else if (e->type == BRO_ENGINE_EVENT_TYPE_STEP_FINISHED)
        {
          of = bro_json_value_get_string (bro_json_value_member (e->data, "job"), "");
          text = g_string_new (NULL);
          g_string_printf (text, "step %s %s", bro_json_value_get_string (bro_json_value_member (e->data, "kind"), ""),
                           bro_json_value_get_string (bro_json_value_member (e->data, "state"), ""));
        }
      else if (e->type == BRO_ENGINE_EVENT_TYPE_JOB_LOG)
        {
          BroJsonValue *ls = bro_json_value_member (e->data, "lines");
          of = bro_json_value_get_string (bro_json_value_member (e->data, "jobId"), "");
          if (strcmp (pipe_name (h, of), who) == 0)
            for (guint k = 0; k < bro_json_value_length (ls); k++)
              g_ptr_array_add (lines, g_strconcat ("log ", bro_json_value_get_string (bro_json_value_member (bro_json_value_at (ls, k), "code"), ""), NULL));
          continue;
        }
      else if (e->type == BRO_ENGINE_EVENT_TYPE_JOB_FINISHED)
        {
          of = bro_json_value_get_string (bro_json_value_member (job, "id"), "");
          text = g_string_new (NULL);
          g_string_printf (text, "finished %s", bro_json_value_get_string (bro_json_value_member (job, "outcome"), ""));
          if ((v = bro_json_value_member (job, "error")))
            g_string_append_printf (text, " %s", bro_json_value_get_string (bro_json_value_member (v, "code"), ""));
        }
      if (text && strcmp (pipe_name (h, of), who) == 0)
        g_ptr_array_add (lines, g_string_free (text, FALSE));
      else if (text)
        g_string_free (text, TRUE);
    }
  return lines;
}

static gboolean
pipeline_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  PipeHarness h = { 0 };
  BroJsonValue *steps_given = bro_json_value_member (given, "steps"), *jobs = bro_json_value_member (given, "jobs"), *ops, *v;
  g_autoptr (GPtrArray) steps = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (BroStepRegistry) registry = NULL;
  g_autoptr (BroStepEnvironment) environment = NULL;
  BroSchedulerLimits limits = { 0, 1, 2, 2 };
  world_init (&h.w);
  gates_init (&h.gates);
  h.failures = failures;
  h.id = id;
  h.bus = bro_event_bus_new (BRO_CLOCK (h.w.clock), 1, 1024, 1024);
  h.scheduler = bro_scheduler_new (h.w.executor, h.bus, &limits);
  h.store = g_object_new (TEST_TYPE_MEMORY_STORE, NULL);
  h.fs = g_object_new (TEST_TYPE_MEMORY_FS, NULL);
  h.config = g_object_new (TEST_TYPE_CASE_CONFIG, NULL);
  if ((v = bro_json_value_member (given, "config")))
    {
      bro_json_value_unref (h.config->document);
      h.config->document = bro_json_value_ref (v);
    }
  for (guint k = 0; k < BRO_STEP_KIND_TRANSCODE + 1; k++)
    {
      TestFakeStep *s = g_object_new (TEST_TYPE_FAKE_STEP, NULL);
      BroJsonValue *b = steps_given ? bro_json_value_member (steps_given, bro_step_kind_to_wire (k)) : NULL;
      s->kind = k;
      s->b = b ? bro_json_value_ref (b) : bro_json_value_new_object ();
      s->gates = &h.gates;
      g_ptr_array_add (steps, s);
    }
  registry = bro_step_registry_new (steps);
  environment = bro_step_environment_new (NULL, BRO_FILE_SYSTEM (h.fs), BRO_CLOCK (h.w.clock), NULL, NULL, NULL,
                                          BRO_CONFIG_SOURCE (h.config), NULL, NULL, "/data");
  h.runner = bro_pipeline_runner_new (h.w.executor, h.bus, h.scheduler, BRO_STORE (h.store), registry, environment, h.w.engine_clock);
  h.names = g_ptr_array_new_with_free_func (g_free);
  h.jobs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  h.outcomes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_decided_free);
  h.started = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  h.events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  bro_engine_executor_post (h.w.executor, pipe_subscribe, &h, NULL);
  bro_engine_executor_barrier (h.w.executor);
  for (guint i = 0; i < bro_json_value_length (jobs); i++)
    {
      BroJsonValue *j = bro_json_value_at (jobs, i), *done;
      const char *name = bro_json_value_get_string (bro_json_value_member (j, "name"), "");
      BroId *jid = g_new0 (BroId, 1);
      g_autoptr (BroJobRecord) r = bro_job_record_new ();
      BroJobKind kind = BRO_JOB_KIND_VIDEO_DISC;
      BroQueue queue = BRO_QUEUE_ACQUISITION;
      BroJsonValue *plan = bro_json_value_member (j, "plan");
      g_snprintf (jid->value, sizeof jid->value, "00000000-0000-4000-8000-%012x", i + 1);
      g_ptr_array_add (h.names, g_strdup (name));
      g_hash_table_insert (h.jobs, g_strdup (name), jid);
      if ((v = bro_json_value_member (j, "kind")))
        bro_job_kind_from_wire (v->string, &kind);
      if ((v = bro_json_value_member (j, "queue")))
        bro_queue_from_wire (v->string, &queue);
      r->id = *jid;
      r->kind = kind;
      r->state = BRO_JOB_STATE_QUEUED;
      r->queue = queue;
      r->title = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "title"), name));
      r->drive_id = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "driveId"), NULL));
      r->automatic = bro_json_value_get_bool (bro_json_value_member (j, "automatic"), FALSE);
      r->request = bro_job_request_new (kind, r->title);
      r->request->automatic = r->automatic;
      r->request->drive_id = g_strdup (r->drive_id);
      r->created_at.unix_milliseconds = START_MS;
      if (plan)
        {
          r->plan = bro_job_plan_new ();
          for (guint k = 0; k < bro_json_value_length (plan); k++)
            {
              BroStepKind sk;
              g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_at (plan, k), ""), &sk));
              g_array_append_val (r->plan->steps, sk);
            }
        }
      bro_job_repository_insert (BRO_JOB_REPOSITORY (h.store), r, NULL);
      done = bro_json_value_member (j, "done");
      for (guint k = 0; done && k < bro_json_value_length (done); k++)
        {
          BroJsonValue *d = bro_json_value_at (done, k), *out = bro_json_value_member (d, "output");
          g_autoptr (BroStepRecord) s = bro_step_record_new ();
          BroStepKind sk;
          g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_member (d, "kind"), ""), &sk));
          s->job_id = *jid;
          s->seq = k;
          s->kind = sk;
          s->state = BRO_STEP_STATE_SUCCEEDED;
          if (bro_json_value_member (d, "state"))
            g_assert_true (bro_step_state_from_wire (bro_json_value_get_string (bro_json_value_member (d, "state"), ""), &s->state));
          s->attempt = 1;
          s->output = bro_step_output_new (sk, out ? bro_json_value_ref (out) : bro_json_value_new_object (), NULL);
          bro_step_repository_save (BRO_STEP_REPOSITORY (h.store), s, NULL);
        }
      if ((done = bro_json_value_member (j, "log")))
        {
          g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_line_dto_free);
          g_autofree char *directory = g_strconcat ("/data/jobs/", jid->value, NULL);
          g_autoptr (GBytes) bytes = NULL;
          g_autofree char *path = NULL;
          for (guint k = 0; k < bro_json_value_length (done); k++)
            {
              BroMessageCode code;
              BroInstant at = { START_MS };
              g_assert_true (bro_message_code_parse (bro_json_value_get_string (bro_json_value_at (done, k), ""), &code));
              g_ptr_array_add (lines, bro_log_line_dto_new (k + 1, at, BRO_SEVERITY_INFO, code, NULL));
            }
          bytes = bro_job_log_encode (lines);
          path = g_strconcat (directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
          bro_file_system_create_directory (BRO_FILE_SYSTEM (h.fs), directory, FALSE, NULL);
          bro_file_system_write_atomically (BRO_FILE_SYSTEM (h.fs), path, bytes, 0600, NULL);
        }
    }
  ops = bro_json_value_member (given, "ops");
  for (guint i = 0; i < bro_json_value_length (ops); i++)
    pipe_run_op (&h, bro_json_value_at (ops, i));

  if ((v = bro_json_value_member (expect, "outcomes")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        const char *name = g_ptr_array_index (v->keys, i);
        BroJsonValue *want = g_ptr_array_index (v->items, i), *e = bro_json_value_member (want, "error");
        BroDecided *d = g_hash_table_lookup (h.outcomes, name);
        g_autofree char *what = g_strdup_printf ("outcome of %s", name);
        bro_test_same_string (failures, id, what, bro_json_value_get_string (bro_json_value_member (want, "outcome"), ""),
                              d ? bro_outcome_to_wire (d->outcome) : NULL);
        if (d && e)
          {
            bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (e, "code"), ""), d->error ? d->error->code : NULL);
            if (bro_json_value_member (e, "params") && d->error)
              bro_test_same_json (failures, id, "error params", bro_json_value_member (e, "params"), d->error->params);
          }
        else if (d)
          bro_test_same_string (failures, id, "error", NULL, d->error ? d->error->code : NULL);
      }
  if ((v = bro_json_value_member (expect, "lines")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        g_autoptr (GPtrArray) got = pipe_lines (&h, g_ptr_array_index (v->keys, i));
        g_autofree char *what = g_strdup_printf ("lines of %s", (char *) g_ptr_array_index (v->keys, i));
        same_lines (failures, id, what, g_ptr_array_index (v->items, i), got);
      }
  if ((v = bro_json_value_member (expect, "queueLines")))
    {
      g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
      for (guint i = 0; i < h.events->len; i++)
        {
          BroEngineEvent *e = g_ptr_array_index (h.events, i);
          BroJsonValue *order = bro_json_value_member (e->data, "order");
          GString *line;
          if (e->type != BRO_ENGINE_EVENT_TYPE_QUEUE_CHANGED)
            continue;
          line = g_string_new (bro_json_value_get_string (bro_json_value_member (e->data, "queue"), ""));
          g_string_append (line, ":");
          for (guint k = 0; k < bro_json_value_length (order); k++)
            g_string_append_printf (line, " %s", pipe_name (&h, bro_json_value_get_string (bro_json_value_at (order, k), "")));
          if (bro_json_value_length (order) == 0)
            g_string_append (line, " ");
          g_ptr_array_add (got, g_string_free (line, FALSE));
        }
      same_lines (failures, id, "queue lines", v, got);
    }
  if ((v = bro_json_value_member (expect, "steps")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        BroId *jid = g_hash_table_lookup (h.jobs, g_ptr_array_index (v->keys, i));
        g_autoptr (GPtrArray) records = steps_of (h.store, jid->value, FALSE);
        g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
        g_autofree char *what = g_strdup_printf ("steps of %s", (char *) g_ptr_array_index (v->keys, i));
        for (guint k = 0; k < records->len; k++)
          {
            BroStepRecord *s = g_ptr_array_index (records, k);
            g_ptr_array_add (got, g_strconcat (bro_step_kind_to_wire (s->kind), " ", bro_step_state_to_wire (s->state), NULL));
          }
        same_lines (failures, id, what, g_ptr_array_index (v->items, i), got);
      }
  if ((v = bro_json_value_member (expect, "records")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        BroId *jid = g_hash_table_lookup (h.jobs, g_ptr_array_index (v->keys, i));
        BroJsonValue *want = g_ptr_array_index (v->items, i);
        g_autoptr (BroJobRecord) r = bro_job_repository_load (BRO_JOB_REPOSITORY (h.store), *jid, NULL);
        g_autoptr (GPtrArray) plan = g_ptr_array_new ();
        for (guint k = 0; r->plan && k < r->plan->steps->len; k++)
          g_ptr_array_add (plan, (gpointer) bro_step_kind_to_wire (g_array_index (r->plan->steps, BroStepKind, k)));
        bro_test_same_string (failures, id, "record state", bro_json_value_get_string (bro_json_value_member (want, "state"), ""),
                              bro_job_state_to_wire (r->state));
        bro_test_same_string (failures, id, "record outcome", bro_json_value_get_string (bro_json_value_member (want, "outcome"), ""),
                              r->has_outcome ? bro_outcome_to_wire (r->outcome) : NULL);
        same_lines (failures, id, "record plan", bro_json_value_member (want, "plan"), plan);
      }
  if ((v = bro_json_value_member (expect, "logFile")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        BroId *jid = g_hash_table_lookup (h.jobs, g_ptr_array_index (v->keys, i));
        g_autofree char *path = g_strconcat ("/data/jobs/", jid->value, "/log.jsonl", NULL);
        g_autoptr (GBytes) bytes = bro_file_system_read (BRO_FILE_SYSTEM (h.fs), path, NULL);
        g_autoptr (GPtrArray) codes = g_ptr_array_new_with_free_func (g_free);
        if (bytes)
          {
            g_autofree char *text = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
            g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
            for (guint k = 0; lines[k]; k++)
              if (*lines[k])
                {
                  g_autoptr (BroJsonValue) line = bro_json_value_parse (lines[k], -1);
                  if (strstr (lines[k], "seq"))
                    bro_test_fail (failures, id, "a log line has a seq");
                  g_ptr_array_add (codes, g_strdup (bro_json_value_get_string (bro_json_value_member (line, "code"), "")));
                }
          }
        same_lines (failures, id, "log file", g_ptr_array_index (v->items, i), codes);
      }
  if ((v = bro_json_value_member (expect, "outputs")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        BroId *jid = g_hash_table_lookup (h.jobs, g_ptr_array_index (v->keys, i));
        BroJsonValue *want = g_ptr_array_index (v->items, i);
        g_autoptr (GPtrArray) records = steps_of (h.store, jid->value, FALSE);
        for (guint k = 0; k < want->keys->len; k++)
          {
            BroStepRecord *found = NULL;
            for (guint n = 0; n < records->len; n++)
              if (strcmp (bro_step_kind_to_wire (((BroStepRecord *) g_ptr_array_index (records, n))->kind), g_ptr_array_index (want->keys, k)) == 0)
                found = g_ptr_array_index (records, n);
            if (!found || !found->output)
              bro_test_fail (failures, id, "no output for %s", (char *) g_ptr_array_index (want->keys, k));
            else
              bro_test_same_json (failures, id, "output", g_ptr_array_index (want->items, k), found->output->data);
          }
      }
  if ((v = bro_json_value_member (expect, "events")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      for (guint i = 0; i < h.events->len; i++)
        bro_json_value_append (got, bro_engine_event_json (g_ptr_array_index (h.events, i)));
      bro_test_same_json (failures, id, "events", v, got);
    }

  bro_engine_executor_stop (h.w.executor);
  g_clear_object (&h.subscription);
  g_object_unref (h.runner);
  g_object_unref (h.scheduler);
  g_object_unref (h.bus);
  g_object_unref (h.store);
  g_object_unref (h.fs);
  g_object_unref (h.config);
  g_ptr_array_unref (h.names);
  g_hash_table_unref (h.jobs);
  g_hash_table_unref (h.outcomes);
  g_hash_table_unref (h.started);
  g_ptr_array_unref (h.events);
  gates_clear (&h.gates);
  world_clear (&h.w);
  return TRUE;
}

static void
test_pipeline_runner (void)
{
  bro_test_run_cases ("engine/pipeline-runner.cases.json", pipeline_case);
}

static gboolean
planner_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BroJsonValue *r = bro_json_value_member (given, "request"), *v;
  BroJobKind kind;
  g_autoptr (BroJobRequest) request = NULL;
  g_autoptr (GArray) steps = NULL;
  g_autoptr (GPtrArray) got = g_ptr_array_new ();
  g_assert_true (bro_job_kind_from_wire (bro_json_value_get_string (bro_json_value_member (r, "kind"), ""), &kind));
  request = bro_job_request_new (kind, bro_json_value_get_string (bro_json_value_member (r, "title"), ""));
  request->automatic = bro_json_value_get_bool (bro_json_value_member (r, "automatic"), FALSE);
  request->drive_id = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "driveId"), NULL));
  if ((v = bro_json_value_member (r, "sessionId")))
    {
      request->session_id = g_new0 (BroId, 1);
      g_strlcpy (request->session_id->value, v->string, sizeof request->session_id->value);
    }
  steps = bro_job_planner_initial (request);
  for (guint i = 0; i < steps->len; i++)
    g_ptr_array_add (got, (gpointer) bro_step_kind_to_wire (g_array_index (steps, BroStepKind, i)));
  same_lines (failures, id, "steps", bro_json_value_member (expect, "steps"), got);
  return TRUE;
}

static void
test_job_planner (void)
{
  bro_test_run_cases ("engine/job-planner.cases.json", planner_case);
}
