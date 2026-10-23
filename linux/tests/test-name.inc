/* test-name.inc: shared/fixtures/engine/name-steps.cases.json: BroNameStep alone, on test-steps.inc's file tree in memory
 * holding the library and the job's staging folder, a clock whose local time is UTC plus an offset, a checkpointer kept
 * between attempts, a drive lookup and the configuration. The format is in claudeDocs/tools/rebuild-phase3/gen-name-cases.py.
 * Included by test-engine.c after test-seal.inc. */

#include "bro-name-step.h"

/* ---- the clock (given.now; local time: UTC plus given.utcOffsetMinutes), the checkpointer and the drive lookup ---- */

#define TEST_TYPE_NAME_FAKES (test_name_fakes_get_type ())
G_DECLARE_FINAL_TYPE (TestNameFakes, test_name_fakes, TEST, NAME_FAKES, GObject)

struct _TestNameFakes {
  GObject parent;
  GMutex lock;
  BroInstant now;
  int utc_offset_minutes;
  BroJsonValue *drives; /* nullable: what the lookup finds */
  BroJsonValue *stored; /* nullable: what the checkpointer holds; an attempt sees what the one before it saved */
  BroJsonValue *saved;  /* nullable: fixed for the attempt, as the runner's is */
  int saves;
};

static BroInstant
name_fakes_now (BroClock *clock)
{
  return TEST_NAME_FAKES (clock)->now;
}

/* Milliseconds cut (floor, negative instants too). */
static BroLocalTime
name_fakes_local_time (BroClock *clock, BroInstant instant)
{
  TestNameFakes *f = TEST_NAME_FAKES (clock);
  gint64 ms = instant.unix_milliseconds;
  gint64 seconds = (ms >= 0 ? ms / 1000 : (ms - 999) / 1000) + (gint64) f->utc_offset_minutes * 60;
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_utc (seconds);
  BroLocalTime local = { 0 };
  if (!t)
    return local;
  local.year = g_date_time_get_year (t);
  local.month = g_date_time_get_month (t);
  local.day = g_date_time_get_day_of_month (t);
  local.hour = g_date_time_get_hour (t);
  local.minute = g_date_time_get_minute (t);
  local.second = g_date_time_get_second (t);
  return local;
}

static BroJsonValue *
name_fakes_saved (BroCheckpointer *checkpointer)
{
  TestNameFakes *f = TEST_NAME_FAKES (checkpointer);
  BroJsonValue *saved;
  g_mutex_lock (&f->lock);
  saved = f->saved ? bro_json_value_ref (f->saved) : NULL;
  g_mutex_unlock (&f->lock);
  return saved;
}

static gboolean
name_fakes_save (BroCheckpointer *checkpointer, const BroJsonValue *checkpoint, BroBroError **error)
{
  TestNameFakes *f = TEST_NAME_FAKES (checkpointer);
  g_autofree char *text = bro_json_value_to_text (checkpoint);
  BroJsonValue *copy = bro_json_value_parse (text, -1);
  g_mutex_lock (&f->lock);
  g_clear_pointer (&f->stored, bro_json_value_unref);
  f->stored = copy;
  f->saves++;
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static BroDriveLocation *
name_fakes_find (BroDriveLookup *lookup, const char *drive_id)
{
  TestNameFakes *f = TEST_NAME_FAKES (lookup);
  BroDriveLocation *at = NULL;
  BroJsonValue *d;
  g_mutex_lock (&f->lock);
  if ((d = bro_json_value_member (f->drives, drive_id)))
    at = bro_drive_location_new ("", STEPS_JS (d, "configId"), "");
  g_mutex_unlock (&f->lock);
  return at;
}

static void
name_fakes_clock_iface (BroClockInterface *i)
{
  i->now = name_fakes_now;
  i->local_time = name_fakes_local_time;
}
static void
name_fakes_checkpointer_iface (BroCheckpointerInterface *i)
{
  i->saved = name_fakes_saved;
  i->save = name_fakes_save;
}
static void name_fakes_lookup_iface (BroDriveLookupInterface *i) { i->find = name_fakes_find; }
G_DEFINE_FINAL_TYPE_WITH_CODE (TestNameFakes, test_name_fakes, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, name_fakes_clock_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CHECKPOINTER, name_fakes_checkpointer_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_LOOKUP, name_fakes_lookup_iface))

static void
test_name_fakes_finalize (GObject *object)
{
  TestNameFakes *f = TEST_NAME_FAKES (object);
  g_clear_pointer (&f->drives, bro_json_value_unref);
  g_clear_pointer (&f->stored, bro_json_value_unref);
  g_clear_pointer (&f->saved, bro_json_value_unref);
  g_mutex_clear (&f->lock);
  G_OBJECT_CLASS (test_name_fakes_parent_class)->finalize (object);
}

static void test_name_fakes_class_init (TestNameFakesClass *k) { G_OBJECT_CLASS (k)->finalize = test_name_fakes_finalize; }
static void test_name_fakes_init (TestNameFakes *f) { g_mutex_init (&f->lock); }

/* ---- the cases ---- */

#define NAME_JOB_ID "00000000-0000-4000-8000-000000000001"

/* given.library / given.staging's entries under @root: a text; a path ending in '/' is an empty folder. */
static void
name_put (TestStepFakes *f, const char *root, const BroJsonValue *entries)
{
  for (guint i = 0; entries && entries->kind == BRO_JSON_VALUE_OBJECT && i < entries->keys->len; i++)
    {
      g_autofree char *path = g_strconcat (root, "/", (char *) entries->keys->pdata[i], NULL);
      const char *text = bro_json_value_get_string (entries->items->pdata[i], "");
      if (g_str_has_suffix (path, "/"))
        {
          path[strlen (path) - 1] = '\0';
          steps_tree_put_dir (f, path);
        }
      else
        steps_tree_put (f, path, g_bytes_new (text, strlen (text)));
    }
}

/* given.cancelAt: the case's cancel, on that operation of that path. */
static void
name_cancel_at (TestStepFakes *f, const char *op, const char *path)
{
  BroJsonValue *at = bro_json_value_member (f->given, "cancelAt");
  if (g_strcmp0 (STEPS_JS (at, "op"), op) == 0 && g_strcmp0 (STEPS_JS (at, "path"), path) == 0)
    bro_cancellation_source_cancel (f->cancel);
}

static void
name_same_error (GPtrArray *failures, const char *id, const char *what, const BroJsonValue *expected, const BroBroError *error)
{
  g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
  g_autofree char *what_params = g_strconcat (what, " params", NULL);
  BroJsonValue *params = bro_json_value_member (expected, "params");
  bro_test_same_string (failures, id, what, STEPS_JS (expected, "code"), error ? error->code : NULL);
  bro_test_same_json (failures, id, what_params, params ? params : none, error ? error->params : none);
}

static gboolean
name_steps_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroStep) step = BRO_STEP (bro_name_step_new ());
  TestStepFakes *fakes = g_object_new (TEST_TYPE_STEP_FAKES, NULL);
  TestNameFakes *name = g_object_new (TEST_TYPE_NAME_FAKES, NULL);
  BroId job_id = { NAME_JOB_ID };
  const char *root = STEPS_JS (bro_json_value_member (bro_json_value_member (given, "libraries"), "lib"), "path");
  const char *library = root ? root : "/lib";
  g_autofree char *staging = g_strconcat (library, "/.bromelia/staging/", NAME_JOB_ID, NULL);
  g_autofree char *directory = g_strconcat ("/data/jobs/", NAME_JOB_ID, NULL);
  BroJsonValue *given_config = bro_json_value_member (given, "config");
  g_autofree char *config_text = given_config ? bro_json_value_to_text (given_config) : NULL;
  g_autoptr (BroConfig) config = bro_config_new (config_text ? bro_json_value_parse (config_text, -1) : bro_json_value_new_object (), NULL);
  g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
  g_autoptr (GPtrArray) sources = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_cancellation_source_free);
  g_autoptr (BroStepEnvironment) environment = NULL;
  g_autoptr (BroStepContext) context = NULL;
  g_autoptr (BroResourceSet) resources = NULL;
  g_autoptr (BroStepReport) report = NULL;
  g_autoptr (BroBroError) thrown = NULL;
  BroJobSnapshot *job;
  BroJsonValue *v, *entries;
  const BroBroError *error;
  int attempts = (int) bro_json_value_get_integer (bro_json_value_member (given, "attempts"), 1);
  gboolean known = TRUE;

  fakes->given = bro_json_value_ref (given);
  fakes->cancel = cancel;
  if (!bro_instant_parse (STEPS_JS (given, "now"), &name->now))
    bro_test_fail (failures, id, "given.now isn't an instant");
  name->utc_offset_minutes = (int) bro_json_value_get_integer (bro_json_value_member (given, "utcOffsetMinutes"), 0);
  if ((v = bro_json_value_member (given, "drives")))
    name->drives = bro_json_value_ref (v);
  if ((v = bro_json_value_member (given, "checkpoint")) && v->kind != BRO_JSON_VALUE_NULL)
    name->stored = bro_json_value_ref (v);
  name_put (fakes, library, bro_json_value_member (given, "library"));
  name_put (fakes, staging, bro_json_value_member (given, "staging"));
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
    bro_cancellation_source_cancel (fakes->cancel); /* before the step runs */
  else if (bro_json_value_member (given, "cancelAt"))
    fakes->on_op = name_cancel_at;

  job = bro_job_snapshot_new (&job_id, steps_request_of (bro_json_value_member (given, "request")), NULL);
  entries = bro_json_value_member (given, "outputs");
  for (guint i = 0; i < bro_json_value_length (entries); i++)
    {
      BroJsonValue *o = bro_json_value_at (entries, i);
      BroStepKind kind;
      if (!bro_step_kind_from_wire (STEPS_JS (o, "kind"), &kind))
        {
          bro_test_fail (failures, id, "unknown step kind %s", STEPS_JS (o, "kind"));
          known = FALSE;
          continue;
        }
      g_ptr_array_add (job->outputs, bro_step_output_new (kind, bro_json_value_ref (bro_json_value_member (o, "data")), NULL));
    }
  job->has_started_at = bro_instant_parse (STEPS_JS (given, "startedAt"), &job->started_at);
  resources = bro_step_resources (step, job);
  environment = bro_step_environment_new (NULL, BRO_FILE_SYSTEM (fakes), BRO_CLOCK (name), NULL, BRO_LIBRARY_PORT (fakes), NULL, NULL,
                                          BRO_DRIVE_LOOKUP (name), NULL, "/data", NULL, "test");
  context = bro_step_context_new (job, bro_cancellation_source_token (cancel), g_object_ref (BRO_PROGRESS_REPORTER (fakes)),
                                  g_object_ref (BRO_JOB_LOGGER (fakes)), g_object_ref (BRO_DECISION_PORT (fakes)),
                                  g_object_ref (BRO_SPACE_RESERVATIONS (fakes)), g_object_ref (BRO_CHECKPOINTER (name)), environment, config,
                                  directory);

  if ((v = bro_json_value_member (expect, "resources")))
    {
      g_autoptr (BroJsonValue) writes = bro_json_value_new_array ();
      for (guint i = 0; resources->library_writes && resources->library_writes[i]; i++)
        bro_json_value_append (writes, bro_json_value_new_string (resources->library_writes[i]));
      bro_test_same_json (failures, id, "resources.libraryWrites", bro_json_value_member (v, "libraryWrites"), writes);
    }
  if ((v = bro_json_value_member (expect, "resumes")))
    bro_test_same_string (failures, id, "resumability", STEPS_JS (v, "resumability"), bro_resumability_to_wire (bro_step_resumability (step)));

  for (int attempt = 1; known && attempt <= MAX (1, attempts); attempt++)
    {
      if (attempt > 1) /* the fails and the hook cleared, a fresh cancel */
        {
          BroCancellationSource *fresh = bro_cancellation_source_new ();
          g_ptr_array_add (sources, fresh);
          g_mutex_lock (&fakes->lock);
          g_ptr_array_set_size (fakes->fails, 0);
          fakes->on_op = NULL;
          g_mutex_unlock (&fakes->lock);
          fakes->cancel = fresh;
          bro_cancellation_token_unref (context->cancel);
          context->cancel = bro_cancellation_token_ref (bro_cancellation_source_token (fresh));
        }
      if (attempt == attempts)
        {
          BroJsonValue *between = bro_json_value_member (given, "between");
          if (attempt > 1 && between)
            {
              name_put (fakes, library, bro_json_value_member (between, "library"));
              name_put (fakes, staging, bro_json_value_member (between, "staging"));
              if ((v = bro_json_value_member (between, "drives")))
                {
                  g_mutex_lock (&name->lock);
                  g_clear_pointer (&name->drives, bro_json_value_unref);
                  name->drives = bro_json_value_ref (v);
                  g_mutex_unlock (&name->lock);
                }
            }
          g_mutex_lock (&fakes->lock);
          g_ptr_array_set_size (fakes->ops, 0);
          g_ptr_array_set_size (fakes->log, 0);
          fakes->recording = TRUE;
          g_mutex_unlock (&fakes->lock);
          name->saves = 0;
        }
      g_mutex_lock (&name->lock);
      g_clear_pointer (&name->saved, bro_json_value_unref);
      name->saved = name->stored ? bro_json_value_ref (name->stored) : NULL;
      g_mutex_unlock (&name->lock);
      g_clear_pointer (&report, bro_step_report_free);
      g_clear_pointer (&thrown, bro_bro_error_free);
      report = bro_step_run (step, context, &thrown);
      fakes->recording = FALSE;
      if (attempt == 1 && attempts > 1)
        {
          error = thrown ? thrown : report ? report->error : NULL;
          if ((v = bro_json_value_member (expect, "firstError")))
            name_same_error (failures, id, "the first attempt's error", v, error);
          else if (error)
            {
              g_autofree char *params = bro_json_value_to_text (error->params);
              bro_test_fail (failures, id, "the first attempt failed: %s %s", error->code, params);
            }
        }
    }
  if (!known)
    goto out;

  error = thrown ? thrown : report ? report->error : NULL;
  if ((v = bro_json_value_member (expect, "error")))
    name_same_error (failures, id, "error", v, error);
  else if (error)
    {
      g_autofree char *params = bro_json_value_to_text (error->params);
      bro_test_fail (failures, id, "unexpected error %s %s", error->code, params);
    }
  if (bro_json_value_get_bool (bro_json_value_member (expect, "skipped"), FALSE) != (report && report->skipped))
    bro_test_fail (failures, id, "skipped: expected %s", bro_json_value_get_bool (bro_json_value_member (expect, "skipped"), FALSE) ? "true" : "false");
  if ((v = bro_json_value_member (expect, "data")))
    {
      g_autoptr (BroJsonValue) null = bro_json_value_new_null ();
      bro_test_same_json (failures, id, "data", v, report && report->data ? report->data : null);
    }
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
          }
      g_string_free (codes, TRUE);
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
  if ((v = bro_json_value_member (expect, "checkpoint")))
    {
      g_autoptr (BroJsonValue) null = bro_json_value_new_null ();
      bro_test_same_json (failures, id, "checkpoint", v, name->stored ? name->stored : null);
    }
  if ((v = bro_json_value_member (expect, "saves")) && bro_json_value_get_integer (v, -1) != name->saves)
    bro_test_fail (failures, id, "saves: expected %" G_GINT64_FORMAT ", got %d", bro_json_value_get_integer (v, -1), name->saves);
out:
  g_clear_pointer (&context, bro_step_context_free);
  g_clear_pointer (&environment, bro_step_environment_free);
  g_object_unref (name);
  g_object_unref (fakes);
  return TRUE;
}

static void
test_name_steps (void)
{
  bro_test_run_cases ("engine/name-steps.cases.json", name_steps_case);
}
