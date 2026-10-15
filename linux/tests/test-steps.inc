/* test-steps.inc: shared/fixtures/engine/video-steps.cases.json: each step alone, with fake ports. Included by
 * test-engine.c (after test-pipeline.inc). */

#include "bro-await-media-step.h"
#include "bro-config-codec.h"
#include "bro-identify-step.h"
#include "bro-listing-builder.h"
#include "bro-probe-step.h"
#include "bro-reconcile-step.h"
#include "bro-release-step.h"
#include "bro-robot.h"

#define STEPS_JS(v, k) bro_json_value_get_string (bro_json_value_member ((v), (k)), NULL)

/* ---- the ports a case gives, recording every call ---- */

#define TEST_TYPE_STEP_FAKES (test_step_fakes_get_type ())
G_DECLARE_FINAL_TYPE (TestStepFakes, test_step_fakes, TEST, STEP_FAKES, GObject)

struct _TestStepFakes {
  GObject parent;
  GMutex lock;
  BroJsonValue *given;
  GPtrArray *calls;    /* char * */
  GPtrArray *log;      /* BroBroMessage * */
  BroJsonValue *progress; /* array */
};

static void
fakes_call (TestStepFakes *f, char *text)
{
  g_mutex_lock (&f->lock);
  g_ptr_array_add (f->calls, text);
  g_mutex_unlock (&f->lock);
}

static BroBroMessage *
fakes_message (BroJsonValue *j, BroSeverity severity)
{
  BroMessageCode code;
  if (!j || !bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (j, "code"), ""), &code))
    return NULL;
  BroJsonValue *params = bro_json_value_member (j, "params");
  return bro_bro_message_new (code, params ? bro_json_value_ref (params) : NULL, severity);
}

static BroDriveLocation *
fakes_find (BroDriveLookup *lookup, const char *drive_id)
{
  TestStepFakes *f = TEST_STEP_FAKES (lookup);
  BroJsonValue *d = bro_json_value_member (bro_json_value_member (f->given, "drives"), drive_id);
  if (!d)
    return NULL;
  BroDriveLocation *at = bro_drive_location_new (bro_json_value_get_string (bro_json_value_member (d, "device"), ""),
                                                 bro_json_value_get_string (bro_json_value_member (d, "configId"), NULL),
                                                 bro_json_value_get_string (bro_json_value_member (d, "label"), ""));
  BroJsonValue *v;
  if ((v = bro_json_value_member (d, "makemkvIndex")) && v->kind == BRO_JSON_VALUE_INTEGER) {
    at->has_makemkv_index = TRUE;
    at->makemkv_index = (int) v->integer;
  }
  if ((v = bro_json_value_member (d, "generation")) && v->kind == BRO_JSON_VALUE_INTEGER) {
    at->has_generation = TRUE;
    at->generation = v->integer;
  }
  if ((v = bro_json_value_member (d, "flags")) && v->kind == BRO_JSON_VALUE_INTEGER) {
    at->has_flags = TRUE;
    at->flags.raw = (int) v->integer;
  }
  return at;
}

static BroMakemkvInvocation *
fakes_invocation (BroMakemkvSetup *setup, const BroEffectiveProfile *profile, const char *selection_override, const char *job_directory,
                  BroBroError **error)
{
  BroJsonValue *min = bro_json_value_member (bro_json_value_member (profile->profile, "makemkv"), "minLengthSeconds");
  g_autofree char *length = min && min->kind == BRO_JSON_VALUE_INTEGER ? g_strdup_printf ("%" G_GINT64_FORMAT, min->integer) : g_strdup ("null");
  fakes_call (TEST_STEP_FAKES (setup), g_strdup_printf ("invocation %s minLength=%s", selection_override ? selection_override : "-", length));
  return bro_makemkv_invocation_new ();
}

static BroListingRun *
fakes_listing (BroMakemkv *makemkv, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation, BroRunSink *sink,
               BroCancellationToken *cancel, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (makemkv);
  fakes_call (f, source->kind == BRO_MAKEMKV_SOURCE_DRIVE ? g_strdup_printf ("listing disc:%d", source->index)
                 : source->kind == BRO_MAKEMKV_SOURCE_ISO ? g_strconcat ("listing iso:", source->path, NULL)
                                                          : g_strconcat ("listing file:", source->path, NULL));
  BroJsonValue *m = bro_json_value_member (f->given, "makemkv"), *v;
  g_autofree char *text = NULL;
  if ((v = bro_json_value_member (m, "listing")))
    text = bro_test_generated_listing (v);
  else
    {
      GString *s = g_string_new (NULL);
      BroJsonValue *robot = bro_json_value_member (m, "robot");
      for (guint i = 0; i < bro_json_value_length (robot); i++)
        g_string_append_printf (s, "%s\n", bro_json_value_get_string (bro_json_value_at (robot, i), ""));
      text = g_string_free (s, FALSE);
    }
  g_autoptr (BroListingBuilder) builder = bro_listing_builder_new ();
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_autoptr (BroRobotEvent) e = bro_robot_parse_line (lines[i]);
      if (e)
        {
          bro_run_sink_event (sink, e);
          bro_listing_builder_feed (builder, e);
        }
    }
  BroJsonValue *o = bro_json_value_member (m, "outcome");
  BroRunOutcome *outcome = g_new0 (BroRunOutcome, 1);
  outcome->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (o, "exitCode"), 0);
  outcome->read_errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  outcome->produced = g_ptr_array_new_with_free_func (g_free);
  if (!bro_status_word_from_wire (bro_json_value_get_string (bro_json_value_member (o, "status"), "success"), &outcome->status))
    outcome->status = BRO_STATUS_WORD_SUCCESS;
  outcome->error = fakes_message (bro_json_value_member (o, "error"), BRO_SEVERITY_ERROR);
  BroListingRun *read = bro_listing_run_new ();
  read->listing = bro_listing_builder_build (builder);
  read->run = bro_makemkv_run_new ();
  read->run->outcome = outcome;
  const char *notice = bro_json_value_get_string (bro_json_value_member (m, "notice"), "");
  BroMakemkvNoticeKind kinds[] = { BRO_MAKEMKV_NOTICE_KEY_EXPIRED, BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED, BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD,
                                   BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED };
  const char *names[] = { "keyExpired", "evaluationNotStarted", "versionTooOld", "libreDriveRequired" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (strcmp (notice, names[i]) == 0)
      read->run->notice = bro_makemkv_notice_new (kinds[i], "");
  read->run->libre_drive = g_strdup (bro_json_value_get_string (bro_json_value_member (m, "libreDrive"), NULL));
  read->run->transcript_problem = fakes_message (bro_json_value_member (m, "transcriptProblem"), BRO_SEVERITY_WARNING);
  read->run->settings_problem = fakes_message (bro_json_value_member (m, "settingsProblem"), BRO_SEVERITY_WARNING);
  read->run->isolation_warning = fakes_message (bro_json_value_member (m, "isolationWarning"), BRO_SEVERITY_WARNING);
  return read;
}

static gboolean
fakes_eject (BroDriveControl *drives, const char *device, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (drives);
  fakes_call (f, g_strconcat ("eject ", device, NULL));
  BroJsonValue *e = bro_json_value_member (f->given, "eject");
  if (!e)
    return TRUE;
  BroJsonValue *params = bro_json_value_member (e, "params");
  bro_bro_error_set (error, bro_json_value_get_string (bro_json_value_member (e, "code"), ""), params ? bro_json_value_ref (params) : NULL);
  return FALSE;
}

static char *
fakes_wait_for_mount (BroDriveControl *drives, const char *device, BroDuration timeout, BroCancellationToken *cancel)
{
  TestStepFakes *f = TEST_STEP_FAKES (drives);
  fakes_call (f, g_strdup_printf ("waitForMount %s %d", device, (int) timeout.seconds));
  return g_strdup (bro_json_value_get_string (bro_json_value_member (f->given, "mount"), NULL));
}

static void
fakes_report (BroProgressReporter *reporter, double fraction, const BroBroMessage *operation, const char *current)
{
  TestStepFakes *f = TEST_STEP_FAKES (reporter);
  BroJsonValue *item = bro_json_value_new_array ();
  bro_json_value_append (item, bro_json_value_new_number (fraction));
  bro_json_value_append (item, bro_json_value_new_string (bro_message_code_wire (operation->code)));
  if (current)
    bro_json_value_append (item, bro_json_value_new_string (current));
  g_mutex_lock (&f->lock);
  bro_json_value_append (f->progress, item);
  g_mutex_unlock (&f->lock);
}

static void
fakes_log (BroJobLogger *logger, const BroBroMessage *message)
{
  TestStepFakes *f = TEST_STEP_FAKES (logger);
  g_mutex_lock (&f->lock);
  g_ptr_array_add (f->log, bro_bro_message_copy (message));
  g_mutex_unlock (&f->lock);
}

static void fakes_lookup_iface (BroDriveLookupInterface *i) { i->find = fakes_find; }
static void fakes_setup_iface (BroMakemkvSetupInterface *i) { i->invocation = fakes_invocation; }
static void fakes_makemkv_iface (BroMakemkvInterface *i) { i->listing = fakes_listing; }
static void
fakes_drives_iface (BroDriveControlInterface *i)
{
  i->eject = fakes_eject;
  i->wait_for_mount = fakes_wait_for_mount;
}
static void fakes_progress_iface (BroProgressReporterInterface *i) { i->report = fakes_report; }
static void fakes_log_iface (BroJobLoggerInterface *i) { i->log = fakes_log; }

G_DEFINE_FINAL_TYPE_WITH_CODE (TestStepFakes, test_step_fakes, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_LOOKUP, fakes_lookup_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV_SETUP, fakes_setup_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV, fakes_makemkv_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, fakes_drives_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_PROGRESS_REPORTER, fakes_progress_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_JOB_LOGGER, fakes_log_iface))

static void
test_step_fakes_finalize (GObject *object)
{
  TestStepFakes *f = TEST_STEP_FAKES (object);
  bro_json_value_unref (f->given);
  g_ptr_array_unref (f->calls);
  g_ptr_array_unref (f->log);
  bro_json_value_unref (f->progress);
  g_mutex_clear (&f->lock);
  G_OBJECT_CLASS (test_step_fakes_parent_class)->finalize (object);
}

static void
test_step_fakes_class_init (TestStepFakesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_step_fakes_finalize;
}

static void
test_step_fakes_init (TestStepFakes *f)
{
  g_mutex_init (&f->lock);
  f->calls = g_ptr_array_new_with_free_func (g_free);
  f->log = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  f->progress = bro_json_value_new_array ();
}

/* ---- the cases ---- */

/* Every key of @expected is in @actual with the same value; arrays have the same length and match element by element;
 * numbers compare as numbers. */
static void
steps_subset (GPtrArray *failures, const char *id, const char *path, const BroJsonValue *expected, const BroJsonValue *actual)
{
  if (expected->kind == BRO_JSON_VALUE_OBJECT)
    {
      if (!actual || actual->kind != BRO_JSON_VALUE_OBJECT)
        {
          bro_test_fail (failures, id, "%s: expected an object", path);
          return;
        }
      for (guint i = 0; i < expected->keys->len; i++)
        {
          g_autofree char *p = g_strconcat (path, ".", (char *) expected->keys->pdata[i], NULL);
          BroJsonValue *a = bro_json_value_member (actual, expected->keys->pdata[i]);
          if (!a)
            bro_test_fail (failures, id, "%s: missing", p);
          else
            steps_subset (failures, id, p, expected->items->pdata[i], a);
        }
    }
  else if (expected->kind == BRO_JSON_VALUE_ARRAY)
    {
      if (!actual || actual->kind != BRO_JSON_VALUE_ARRAY || actual->items->len != expected->items->len)
        {
          bro_test_same_json (failures, id, path, expected, actual);
          return;
        }
      for (guint i = 0; i < expected->items->len; i++)
        {
          g_autofree char *p = g_strdup_printf ("%s[%u]", path, i);
          steps_subset (failures, id, p, expected->items->pdata[i], actual->items->pdata[i]);
        }
    }
  else if ((expected->kind == BRO_JSON_VALUE_INTEGER || expected->kind == BRO_JSON_VALUE_NUMBER) && actual
           && (actual->kind == BRO_JSON_VALUE_INTEGER || actual->kind == BRO_JSON_VALUE_NUMBER))
    {
      if (bro_json_value_get_number (expected, 0) != bro_json_value_get_number (actual, 0))
        bro_test_same_json (failures, id, path, expected, actual);
    }
  else
    bro_test_same_json (failures, id, path, expected, actual);
}

static BroJobRequest *
steps_request_of (const BroJsonValue *r)
{
  BroJobKind kind;
  if (!bro_job_kind_from_wire (bro_json_value_get_string (bro_json_value_member (r, "kind"), ""), &kind))
    kind = BRO_JOB_KIND_VIDEO_DISC;
  BroJobRequest *q = bro_job_request_new (kind, bro_json_value_get_string (bro_json_value_member (r, "title"), ""));
  q->automatic = bro_json_value_get_bool (bro_json_value_member (r, "automatic"), FALSE);
  q->drive_id = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "driveId"), NULL));
  BroJsonValue *v;
  if ((v = bro_json_value_member (r, "mediaGeneration")) && v->kind == BRO_JSON_VALUE_INTEGER)
    {
      q->has_media_generation = TRUE;
      q->media_generation = v->integer;
    }
  if ((v = bro_json_value_member (r, "sessionId")) && v->kind == BRO_JSON_VALUE_STRING)
    {
      q->session_id = g_new0 (BroId, 1);
      g_strlcpy (q->session_id->value, v->string, sizeof q->session_id->value);
    }
  if ((v = bro_json_value_member (r, "options")))
    q->options = bro_json_value_ref (v);
  if ((v = bro_json_value_member (r, "details")))
    q->details = bro_json_value_ref (v);
  return q;
}

static BroStep *
steps_step (const char *kind)
{
  if (strcmp (kind, "awaitMedia") == 0)
    return BRO_STEP (bro_await_media_step_new ());
  if (strcmp (kind, "probe") == 0)
    return BRO_STEP (bro_probe_step_new ());
  if (strcmp (kind, "reconcile") == 0)
    return BRO_STEP (bro_reconcile_step_new ());
  if (strcmp (kind, "identify") == 0)
    return BRO_STEP (bro_identify_step_new ());
  if (strcmp (kind, "release") == 0)
    return BRO_STEP (bro_release_step_new ());
  return NULL;
}

static const char *
steps_notice_name (const BroMakemkvNotice *n)
{
  if (!n)
    return NULL;
  switch (n->kind)
    {
    case BRO_MAKEMKV_NOTICE_KEY_EXPIRED: return "keyExpired";
    case BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED: return "evaluationNotStarted";
    case BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD: return "versionTooOld";
    case BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED: return "libreDriveRequired";
    case BRO_MAKEMKV_NOTICE_LIBRE_DRIVE: return "libreDrive";
    }
  return NULL;
}

static gboolean
video_steps_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroStep) step = steps_step (bro_json_value_get_string (bro_json_value_member (given, "step"), ""));
  if (!step)
    return FALSE;
  TestStepFakes *fakes = g_object_new (TEST_TYPE_STEP_FAKES, NULL);
  fakes->given = bro_json_value_ref (given);
  BroJsonValue *config_json = bro_json_value_member (given, "config"), *v;
  g_autoptr (BroJsonValue) default_config = bro_json_value_new_object ();
  bro_json_value_set (default_config, "version", bro_json_value_new_integer (3));
  g_autofree char *config_text = bro_json_value_to_text (config_json ? config_json : default_config);
  g_autoptr (BroConfig) config = bro_config_codec_decode (config_text, -1, NULL);
  BroId job_id = { "00000000-0000-4000-8000-000000000001" };
  BroJobSnapshot *job = bro_job_snapshot_new (&job_id, steps_request_of (bro_json_value_member (given, "request")), NULL);
  BroJsonValue *outputs = bro_json_value_member (given, "outputs");
  for (guint i = 0; i < bro_json_value_length (outputs); i++)
    {
      BroJsonValue *o = bro_json_value_at (outputs, i);
      BroStepKind kind;
      g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_member (o, "kind"), ""), &kind));
      g_ptr_array_add (job->outputs, bro_step_output_new (kind, bro_json_value_ref (bro_json_value_member (o, "data")), NULL));
    }
  g_autoptr (BroToolSet) tools = bro_tool_set_new (BRO_MAKEMKV (fakes), NULL);
  g_autoptr (BroStepEnvironment) environment = bro_step_environment_new (tools, NULL, NULL, BRO_DRIVE_CONTROL (fakes), NULL, NULL, NULL,
                                                                         BRO_DRIVE_LOOKUP (fakes), BRO_MAKEMKV_SETUP (fakes), "/data");
  g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
  g_autofree char *directory = g_strconcat ("/data/jobs/", job_id.value, NULL);
  g_autoptr (BroResourceSet) resources = bro_step_resources (step, job);
  g_autoptr (BroStepContext) context = bro_step_context_new (job, bro_cancellation_source_token (cancel), g_object_ref (BRO_PROGRESS_REPORTER (fakes)),
                                                             g_object_ref (BRO_JOB_LOGGER (fakes)), NULL, environment, config, directory);

  if ((v = bro_json_value_member (expect, "resources")))
    {
      g_autoptr (BroJsonValue) drives = bro_json_value_new_array ();
      for (guint i = 0; resources->drives && resources->drives[i]; i++)
        bro_json_value_append (drives, bro_json_value_new_string (resources->drives[i]));
      bro_test_same_json (failures, id, "resources.drives", bro_json_value_member (v, "drives"), drives);
    }
  g_autoptr (BroBroError) thrown = NULL;
  g_autoptr (BroStepReport) report = bro_step_run (step, context, &thrown);
  const BroBroError *error = thrown ? thrown : report ? report->error : NULL;
  if ((v = bro_json_value_member (expect, "error")))
    {
      bro_test_same_string (failures, id, "error", STEPS_JS (v, "code"), error ? error->code : NULL);
      g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
      BroJsonValue *params = bro_json_value_member (v, "params");
      bro_test_same_json (failures, id, "error params", params ? params : none, error ? error->params : none);
    }
  else if (error)
    bro_test_fail (failures, id, "unexpected error %s", error->code);
  if ((v = bro_json_value_member (expect, "notice")))
    bro_test_same_string (failures, id, "notice", v->string, steps_notice_name (report ? report->notice : NULL));
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
            g_autofree char *what = g_strdup_printf ("log[%u]", i);
            bro_test_same_string (failures, id, what, e->kind == BRO_JSON_VALUE_STRING ? e->string : STEPS_JS (e, "code"),
                                  bro_message_code_wire (got->code));
            if (e->kind == BRO_JSON_VALUE_OBJECT)
              {
                g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
                BroJsonValue *params = bro_json_value_member (e, "params");
                g_autofree char *what_params = g_strdup_printf ("log[%u] params", i);
                bro_test_same_json (failures, id, what_params, params ? params : none, got->params);
                if (STEPS_JS (e, "severity"))
                  bro_test_same_string (failures, id, what, STEPS_JS (e, "severity"), bro_severity_to_wire (got->severity));
              }
          }
      g_string_free (codes, TRUE);
    }
  if ((v = bro_json_value_member (expect, "calls")))
    {
      GString *want = g_string_new (NULL), *got = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_string_append_printf (want, "%s%s", i ? " | " : "", bro_json_value_get_string (bro_json_value_at (v, i), ""));
      for (guint i = 0; i < fakes->calls->len; i++)
        g_string_append_printf (got, "%s%s", i ? " | " : "", (char *) fakes->calls->pdata[i]);
      bro_test_same_string (failures, id, "calls", want->str, got->str);
      g_string_free (want, TRUE);
      g_string_free (got, TRUE);
    }
  if ((v = bro_json_value_member (expect, "progress")))
    steps_subset (failures, id, "progress", v, fakes->progress);
  g_clear_pointer (&context, bro_step_context_free);
  g_object_unref (fakes);
  return TRUE;
}

static void
test_video_steps (void)
{
  bro_test_run_cases ("engine/video-steps.cases.json", video_steps_case);
}
