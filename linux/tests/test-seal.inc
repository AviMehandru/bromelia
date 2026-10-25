/* test-seal.inc: shared/fixtures/engine/seal-steps.cases.json: BroSealStep alone, on test-steps.inc's file tree in memory
 * (a disk and a cache, failures, a cancel on an operation), a fixed clock and the harness's own renderer. The format is in
 * claudeDocs/tools/rebuild-phase3/gen-seal-cases.py. Included by test-engine.c after test-steps.inc. */

#include "bro-seal-step.h"

/* ---- a Clock at given.now ---- */

#define TEST_TYPE_SEAL_CLOCK (test_seal_clock_get_type ())
G_DECLARE_FINAL_TYPE (TestSealClock, test_seal_clock, TEST, SEAL_CLOCK, GObject)

struct _TestSealClock {
  GObject parent;
  BroInstant now;
};

static BroInstant
seal_clock_now (BroClock *clock)
{
  return TEST_SEAL_CLOCK (clock)->now;
}

static void seal_clock_iface (BroClockInterface *i) { i->now = seal_clock_now; }
G_DEFINE_FINAL_TYPE_WITH_CODE (TestSealClock, test_seal_clock, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, seal_clock_iface))
static void test_seal_clock_class_init (TestSealClockClass *k) {}
static void test_seal_clock_init (TestSealClock *c) {}

/* ---- a MessageRenderer: '<code>' then ' <key>=<value>' per param: strings as they are, other values as canonical JSON
 * without its final newline ---- */

#define TEST_TYPE_SEAL_RENDERER (test_seal_renderer_get_type ())
G_DECLARE_FINAL_TYPE (TestSealRenderer, test_seal_renderer, TEST, SEAL_RENDERER, GObject)

struct _TestSealRenderer {
  GObject parent;
};

static char *
seal_render (BroMessageRenderer *r, const BroBroMessage *message)
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
          gsize n = g_bytes_get_size (bytes);
          const char *json = g_bytes_get_data (bytes, NULL);
          g_string_append_len (text, json, n > 0 && json[n - 1] == '\n' ? n - 1 : n);
        }
    }
  return g_string_free (text, FALSE);
}

static void seal_renderer_iface (BroMessageRendererInterface *i) { i->render = seal_render; }
G_DEFINE_FINAL_TYPE_WITH_CODE (TestSealRenderer, test_seal_renderer, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MESSAGE_RENDERER, seal_renderer_iface))
static void test_seal_renderer_class_init (TestSealRendererClass *k) {}
static void test_seal_renderer_init (TestSealRenderer *r) {}

/* ---- the cases ---- */

#define SEAL_JOB_ID "00000000-0000-4000-8000-000000000001"

/* given.staging's entries: a text (on disk), or {disk, cache}; a path ending in '/' is an empty folder. */
static void
seal_put_staging (TestStepFakes *f, const char *staging, const BroJsonValue *entries)
{
  for (guint i = 0; entries && entries->kind == BRO_JSON_VALUE_OBJECT && i < entries->keys->len; i++)
    {
      const BroJsonValue *content = entries->items->pdata[i];
      g_autofree char *path = g_strconcat (staging, "/", (char *) entries->keys->pdata[i], NULL);
      const char *disk = content->kind == BRO_JSON_VALUE_STRING ? content->string : STEPS_JS (content, "disk");
      if (g_str_has_suffix (path, "/"))
        {
          path[strlen (path) - 1] = '\0';
          steps_tree_put_dir (f, path);
          continue;
        }
      steps_tree_put (f, path, g_bytes_new (disk, strlen (disk)));
      if (content->kind == BRO_JSON_VALUE_OBJECT)
        {
          const char *cache = STEPS_JS (content, "cache");
          g_mutex_lock (&f->lock);
          g_hash_table_replace (f->cached, g_strdup (path), g_bytes_new (cache, strlen (cache)));
          g_mutex_unlock (&f->lock);
        }
    }
}

/* given.cancelAt: the case's cancel, on that operation of that path. */
static void
seal_cancel_at (TestStepFakes *f, const char *op, const char *path)
{
  BroJsonValue *at = bro_json_value_member (f->given, "cancelAt");
  if (g_strcmp0 (STEPS_JS (at, "op"), op) == 0 && g_strcmp0 (STEPS_JS (at, "path"), path) == 0)
    bro_cancellation_source_cancel (f->cancel);
}

/* Every file under staging (not the folders) → its text; empty when staging isn't there. */
static GHashTable *
seal_snapshot (TestStepFakes *f, const char *staging)
{
  GHashTable *files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GPtrArray) tree = bro_file_system_exists (BRO_FILE_SYSTEM (f), staging) ? bro_file_tree_files (BRO_FILE_SYSTEM (f), staging, &error) : NULL;
  for (guint i = 0; tree && i < tree->len; i++)
    {
      const char *rel = tree->pdata[i];
      g_autofree char *path = g_strconcat (staging, "/", rel, NULL);
      g_autoptr (BroBroError) unread = NULL;
      g_autoptr (GBytes) bytes = NULL;
      if (g_str_has_suffix (rel, "/") || !(bytes = steps_fakes_read (BRO_FILE_SYSTEM (f), path, &unread)))
        continue;
      g_hash_table_replace (files, g_strdup (rel), g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes)));
    }
  return files;
}

static int
seal_by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

/* The keys of @files, sorted, one per line. */
static char *
seal_names (GHashTable *files)
{
  g_autoptr (GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (files);
  GString *text = g_string_new (NULL);
  g_ptr_array_sort (keys, seal_by_text);
  for (guint i = 0; i < keys->len; i++)
    g_string_append_printf (text, "%s%s", i ? "\n" : "", (char *) keys->pdata[i]);
  return g_string_free (text, FALSE);
}

/* What another step's harness adds to the Seal one (test-keep-apart.inc): the step, and an object that is its library
 * port and checkpointer, set up with the case's fakes and cancel, told when a run starts (last: the recorded counts are
 * cleared), and that checks its own expectations. NULL for SealStep. */
typedef struct {
  BroStep *step;
  GObject *port; /* BroLibraryPort and BroCheckpointer */
  void (*setup) (GObject *port, TestStepFakes *fakes, BroCancellationSource *cancel);
  void (*start_run) (GObject *port, gboolean last);
  void (*check) (GObject *port, const char *id, BroJsonValue *expect, GPtrArray *failures);
} SealHooks;

static gboolean
seal_case_run (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures, const SealHooks *hooks)
{
  g_autoptr (BroStep) step = hooks ? g_object_ref (hooks->step) : BRO_STEP (bro_seal_step_new ());
  TestStepFakes *fakes = g_object_new (TEST_TYPE_STEP_FAKES, NULL);
  TestSealClock *clock = g_object_new (TEST_TYPE_SEAL_CLOCK, NULL);
  TestSealRenderer *renderer = g_object_new (TEST_TYPE_SEAL_RENDERER, NULL);
  BroId job_id = { SEAL_JOB_ID };
  const char *root = STEPS_JS (bro_json_value_member (bro_json_value_member (given, "libraries"), "lib"), "path");
  g_autofree char *staging = g_strconcat (root ? root : "/lib", "/.bromelia/staging/", SEAL_JOB_ID, NULL);
  g_autofree char *directory = g_strconcat ("/data/jobs/", SEAL_JOB_ID, NULL);
  g_autoptr (BroJsonValue) default_config = bro_json_value_new_object ();
  g_autofree char *config_text = NULL;
  g_autoptr (BroConfig) config = NULL;
  g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
  g_autoptr (BroStepEnvironment) environment = NULL;
  g_autoptr (BroStepContext) context = NULL;
  g_autoptr (BroResourceSet) resources = NULL;
  g_autoptr (GHashTable) first = NULL;
  g_autoptr (GHashTable) after = NULL;
  g_autoptr (BroStepReport) report = NULL;
  g_autoptr (BroBroError) thrown = NULL;
  BroJobSnapshot *job;
  BroJsonValue *v, *entries;
  const BroBroError *error;
  int runs = (int) bro_json_value_get_integer (bro_json_value_member (given, "runs"), 1);

  fakes->given = bro_json_value_ref (given);
  fakes->cancel = cancel;
  g_assert_true (bro_instant_parse (STEPS_JS (given, "now"), &clock->now));
  seal_put_staging (fakes, staging, bro_json_value_member (given, "staging"));
  entries = bro_json_value_member (given, "job");
  for (guint i = 0; entries && i < entries->keys->len; i++)
    {
      g_autofree char *path = g_strconcat (directory, "/", (char *) entries->keys->pdata[i], NULL);
      const char *text = bro_json_value_get_string (entries->items->pdata[i], "");
      steps_tree_put (fakes, path, g_bytes_new (text, strlen (text)));
    }
  entries = bro_json_value_member (given, "fails");
  for (guint i = 0; i < bro_json_value_length (entries); i++)
    {
      BroJsonValue *f = bro_json_value_at (entries, i);
      BroJsonValue *why = bro_json_value_member (f, "error");
      BroJsonValue *params = bro_json_value_member (why, "params");
      StepsFsFail *fail = g_new0 (StepsFsFail, 1);
      fail->op = g_strdup (STEPS_JS (f, "op"));
      fail->path = g_strdup (STEPS_JS (f, "path"));
      fail->error = bro_bro_error_new (STEPS_JS (why, "code"), params ? bro_json_value_ref (params) : NULL, NULL);
      g_ptr_array_add (fakes->fails, fail);
    }
  if (g_strcmp0 (STEPS_JS (bro_json_value_member (given, "cancelAt"), "op"), "start") == 0)
    bro_cancellation_source_cancel (cancel); /* the job is cancelled before the step runs */
  else if (bro_json_value_member (given, "cancelAt"))
    fakes->on_op = seal_cancel_at;
  if (hooks)
    hooks->setup (hooks->port, fakes, cancel);

  job = bro_job_snapshot_new (&job_id, steps_request_of (bro_json_value_member (given, "request")), NULL);
  entries = bro_json_value_member (given, "outputs");
  for (guint i = 0; i < bro_json_value_length (entries); i++)
    {
      BroJsonValue *o = bro_json_value_at (entries, i);
      BroStepKind kind;
      BroStepOutput *output;
      g_assert_true (bro_step_kind_from_wire (STEPS_JS (o, "kind"), &kind));
      output = bro_step_output_new (kind, bro_json_value_ref (bro_json_value_member (o, "data")), NULL);
      output->read_errors = (int) bro_json_value_get_integer (bro_json_value_member (o, "readErrors"), 0);
      g_ptr_array_add (job->outputs, output);
    }
  job->has_started_at = bro_instant_parse (STEPS_JS (given, "startedAt"), &job->started_at);
  resources = bro_step_resources (step, job);

  bro_json_value_set (default_config, "version", bro_json_value_new_integer (3));
  config_text = bro_json_value_to_text (default_config);
  config = bro_config_codec_decode (config_text, -1, NULL);
  environment = bro_step_environment_new (NULL, BRO_FILE_SYSTEM (fakes), BRO_CLOCK (clock), NULL,
                                          hooks ? BRO_LIBRARY_PORT (hooks->port) : BRO_LIBRARY_PORT (fakes), NULL, NULL, NULL, NULL, "/data",
                                          BRO_MESSAGE_RENDERER (renderer), STEPS_JS (given, "appVersion"));
  context = bro_step_context_new (job, bro_cancellation_source_token (cancel), g_object_ref (BRO_PROGRESS_REPORTER (fakes)),
                                  g_object_ref (BRO_JOB_LOGGER (fakes)), g_object_ref (BRO_DECISION_PORT (fakes)),
                                  g_object_ref (BRO_SPACE_RESERVATIONS (fakes)),
                                  g_object_ref (hooks ? BRO_CHECKPOINTER (hooks->port) : BRO_CHECKPOINTER (fakes)), environment, config, directory);

  if ((v = bro_json_value_member (expect, "resources")))
    {
      g_autoptr (BroJsonValue) writes = bro_json_value_new_array ();
      for (guint i = 0; resources->library_writes && resources->library_writes[i]; i++)
        bro_json_value_append (writes, bro_json_value_new_string (resources->library_writes[i]));
      bro_test_same_json (failures, id, "resources.libraryWrites", bro_json_value_member (v, "libraryWrites"), writes);
    }
  if ((v = bro_json_value_member (expect, "resumes")))
    bro_test_same_string (failures, id, "resumability", STEPS_JS (v, "resumability"), bro_resumability_to_wire (bro_step_resumability (step)));

  for (int run = 1; run <= runs; run++)
    {
      if (run == runs)
        {
          if (run > 1)
            seal_put_staging (fakes, staging, bro_json_value_member (given, "between"));
          g_mutex_lock (&fakes->lock);
          g_ptr_array_set_size (fakes->ops, 0);
          g_hash_table_remove_all (fakes->reads);
          g_ptr_array_set_size (fakes->log, 0);
          bro_json_value_unref (fakes->progress);
          fakes->progress = bro_json_value_new_array ();
          fakes->recording = TRUE;
          g_mutex_unlock (&fakes->lock);
        }
      if (hooks)
        hooks->start_run (hooks->port, run == runs && run > 1);
      g_clear_pointer (&report, bro_step_report_free);
      g_clear_pointer (&thrown, bro_bro_error_free);
      report = bro_step_run (step, context, &thrown);
      fakes->recording = FALSE;
      if (run == 1 && runs > 1)
        {
          error = thrown ? thrown : report ? report->error : NULL;
          if (error)
            {
              bro_test_fail (failures, id, "the first run failed: %s", error->code);
              goto out;
            }
          first = seal_snapshot (fakes, staging);
        }
    }

  error = thrown ? thrown : report ? report->error : NULL;
  if ((v = bro_json_value_member (expect, "error")))
    {
      g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
      BroJsonValue *params = bro_json_value_member (v, "params");
      bro_test_same_string (failures, id, "error", STEPS_JS (v, "code"), error ? error->code : NULL);
      bro_test_same_json (failures, id, "error params", params ? params : none, error ? error->params : none);
    }
  else if (error)
    {
      g_autofree char *params = bro_json_value_to_text (error->params);
      bro_test_fail (failures, id, "unexpected error %s %s", error->code, params);
    }
  if (bro_json_value_get_bool (bro_json_value_member (expect, "skipped"), FALSE) != (report && report->skipped))
    bro_test_fail (failures, id, "skipped: expected %s", bro_json_value_get_bool (bro_json_value_member (expect, "skipped"), FALSE) ? "true" : "false");
  if ((v = bro_json_value_member (expect, "data")))
    steps_subset (failures, id, "data", v, report ? report->data : NULL);
  if ((v = bro_json_value_member (expect, "log")))
    {
      GString *codes = g_string_new (NULL);
      for (guint i = 0; i < fakes->log->len; i++)
        g_string_append_printf (codes, "%s%s", i ? ", " : "", bro_message_code_wire (((BroBroMessage *) fakes->log->pdata[i])->code));
      if (bro_json_value_length (v) != fakes->log->len)
        bro_test_fail (failures, id, "log: expected %u lines, got %u (%s)", bro_json_value_length (v), fakes->log->len, codes->str);
      else
        for (guint i = 0; i < fakes->log->len; i++)
          {
            BroJsonValue *e = bro_json_value_at (v, i);
            const BroBroMessage *got = fakes->log->pdata[i];
            g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
            BroJsonValue *params = bro_json_value_member (e, "params");
            g_autofree char *what = g_strdup_printf ("log[%u]", i);
            g_autofree char *what_params = g_strdup_printf ("log[%u] params", i);
            bro_test_same_string (failures, id, what, STEPS_JS (e, "code"), bro_message_code_wire (got->code));
            bro_test_same_json (failures, id, what_params, params ? params : none, got->params);
            if (STEPS_JS (e, "severity"))
              bro_test_same_string (failures, id, what, STEPS_JS (e, "severity"), bro_severity_to_wire (got->severity));
          }
      g_string_free (codes, TRUE);
    }
  if ((v = bro_json_value_member (expect, "progress")))
    {
      if (bro_json_value_length (v) != bro_json_value_length (fakes->progress))
        bro_test_fail (failures, id, "progress length: expected %u, got %u", bro_json_value_length (v), bro_json_value_length (fakes->progress));
      else
        steps_subset (failures, id, "progress", v, fakes->progress);
    }
  if ((v = bro_json_value_member (expect, "fs")))
    {
      GString *want = g_string_new (NULL), *got = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_string_append_printf (want, "%s%s", i ? "\n" : "", bro_json_value_get_string (bro_json_value_at (v, i), ""));
      for (guint i = 0; i < fakes->ops->len; i++)
        g_string_append_printf (got, "%s%s", i ? "\n" : "", (char *) fakes->ops->pdata[i]);
      bro_test_same_string (failures, id, "fs", want->str, got->str);
      g_string_free (want, TRUE);
      g_string_free (got, TRUE);
    }
  if (fakes->open_streams != 0) /* every case: a handle left open blocks Commit's move on Windows */
    bro_test_fail (failures, id, "streams left open: %d", fakes->open_streams);
  if ((v = bro_json_value_member (expect, "reads")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        g_autofree char *path = g_strconcat (staging, "/", (char *) v->keys->pdata[i], NULL);
        gint64 want = bro_json_value_get_integer (v->items->pdata[i], -1);
        gint64 got = GPOINTER_TO_INT (g_hash_table_lookup (fakes->reads, path));
        if (want != got)
          bro_test_fail (failures, id, "reads of %s: expected %" G_GINT64_FORMAT ", got %" G_GINT64_FORMAT, (char *) v->keys->pdata[i], want, got);
      }
  after = seal_snapshot (fakes, staging);
  if ((v = bro_json_value_member (expect, "tree")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      g_autoptr (BroBroError) unlisted = NULL;
      g_autoptr (GPtrArray) files = bro_file_system_exists (BRO_FILE_SYSTEM (fakes), staging)
                                        ? bro_file_tree_files (BRO_FILE_SYSTEM (fakes), staging, &unlisted)
                                        : g_ptr_array_new ();
      for (guint i = 0; files && i < files->len; i++)
        bro_json_value_append (got, bro_json_value_new_string (files->pdata[i]));
      bro_test_same_json (failures, id, "tree", v, got);
    }
  if ((v = bro_json_value_member (expect, "contents")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        g_autofree char *what = g_strconcat ("contents of ", (char *) v->keys->pdata[i], NULL);
        bro_test_same_string (failures, id, what, bro_json_value_get_string (v->items->pdata[i], NULL), g_hash_table_lookup (after, v->keys->pdata[i]));
      }
  if ((v = bro_json_value_member (expect, "record")) && v->kind == BRO_JSON_VALUE_STRING)
    {
      const char *name = report ? STEPS_JS (report->data, "record") : NULL;
      g_autofree char *what = g_strconcat ("record ", name ? name : "", NULL);
      bro_test_same_string (failures, id, what, v->string, name ? g_hash_table_lookup (after, name) : NULL);
    }
  if (hooks)
    hooks->check (hooks->port, id, expect, failures);
  if (bro_json_value_get_bool (bro_json_value_member (expect, "sameAsFirst"), FALSE))
    {
      g_autofree char *before_names = seal_names (first);
      g_autofree char *after_names = seal_names (after);
      GHashTableIter it;
      gpointer key, value;
      bro_test_same_string (failures, id, "the files after the rerun", before_names, after_names);
      g_hash_table_iter_init (&it, first);
      while (g_hash_table_iter_next (&it, &key, &value))
        {
          g_autofree char *what = g_strconcat ("the rerun's ", (char *) key, NULL);
          bro_test_same_string (failures, id, what, value, g_hash_table_lookup (after, key));
        }
    }
out:
  g_clear_pointer (&context, bro_step_context_free);
  g_clear_pointer (&environment, bro_step_environment_free);
  g_object_unref (renderer);
  g_object_unref (clock);
  g_object_unref (fakes);
  return TRUE;
}

static gboolean
seal_steps_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  return seal_case_run (id, given, expect, failures, NULL);
}

static void
test_seal_steps (void)
{
  bro_test_run_cases ("engine/seal-steps.cases.json", seal_steps_case);
}
