/* test-commit.inc: shared/fixtures/engine/commit-steps.cases.json: BroCommitStep (or BroNotifyStep) alone, with a fake
 * library port that records what it is asked to commit, test-seal.inc's fixed clock, and test-steps.inc's fakes for the
 * log and progress (nothing touches a file system). The format is in claudeDocs/tools/rebuild-phase3/gen-commit-cases.py.
 * Included by test-engine.c after test-name.inc. */

#include "bro-commit-step.h"
#include "bro-notify-step.h"

/* ---- a LibraryPort that records the commits it is asked for ---- */

#define TEST_TYPE_COMMIT_PORT (test_commit_port_get_type ())
G_DECLARE_FINAL_TYPE (TestCommitPort, test_commit_port, TEST, COMMIT_PORT, GObject)

struct _TestCommitPort {
  GObject parent;
  GMutex lock;
  BroJsonValue *given;
  BroJsonValue *calls;                  /* array */
  int staging_asked;
  BroCancellationSource *cancel_on_call; /* the job's, cancelled as commit or quarantine is called; not owned; nullable */
};

/* given's {code, params} as an error. */
static BroBroError *
commit_error_of (BroJsonValue *why)
{
  BroJsonValue *params = bro_json_value_member (why, "params");
  return bro_bro_error_new (STEPS_JS (why, "code"), params ? bro_json_value_ref (params) : NULL, NULL);
}

static BroLibraryHandle *
commit_port_require (BroLibraryPort *port, const char *library_id, BroBroError **error)
{
  TestCommitPort *p = TEST_COMMIT_PORT (port);
  BroJsonValue *l = bro_json_value_member (bro_json_value_member (p->given, "libraries"), library_id);
  BroJsonValue *why = bro_json_value_member (l, "offline");
  if (!l)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_LIBRARY_NOT_FOUND), NULL);
      return NULL;
    }
  if (why)
    {
      *error = commit_error_of (why);
      return NULL;
    }
  return bro_library_handle_new (library_id, STEPS_JS (l, "path"));
}

static char *
commit_port_staging_directory (BroLibraryPort *port, const char *library_id, const BroId *job_id, BroBroError **error)
{
  TestCommitPort *p = TEST_COMMIT_PORT (port);
  g_autoptr (BroLibraryHandle) library = commit_port_require (port, library_id, error);
  if (!library)
    return NULL;
  g_mutex_lock (&p->lock);
  p->staging_asked++;
  g_mutex_unlock (&p->lock);
  return g_strconcat (library->path, "/.bromelia/staging/", job_id->value, NULL);
}

static BroJsonValue *
commit_n (gboolean has, int v)
{
  return has ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

static BroJsonValue *
commit_s (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

static BroJsonValue *
commit_instant (BroInstant at)
{
  g_autofree char *text = bro_instant_format (at);
  return bro_json_value_new_string (text);
}

/* BroUnitRecord's fields in its order; enums by their wire names, instants formatted. */
static BroJsonValue *
commit_unit_json (const BroUnitRecord *u)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "id", bro_json_value_new_string (u->id.value));
  bro_json_value_set (o, "libraryId", bro_json_value_new_string (u->library_id));
  bro_json_value_set (o, "physicalDiscId", u->has_physical_disc_id ? bro_json_value_new_string (u->physical_disc_id.value) : bro_json_value_new_null ());
  bro_json_value_set (o, "jobId", u->has_job_id ? bro_json_value_new_string (u->job_id.value) : bro_json_value_new_null ());
  bro_json_value_set (o, "path", bro_json_value_new_string (u->path));
  bro_json_value_set (o, "recordFile", bro_json_value_new_string (u->record_file));
  bro_json_value_set (o, "recordVersion", bro_json_value_new_integer (u->record_version));
  bro_json_value_set (o, "state", bro_json_value_new_string (bro_unit_state_to_wire (u->state)));
  bro_json_value_set (o, "status", bro_json_value_new_string (bro_unit_status_to_wire (u->status)));
  bro_json_value_set (o, "name", bro_json_value_new_string (u->name));
  bro_json_value_set (o, "kind", bro_json_value_new_string (bro_media_kind_to_wire (u->kind)));
  bro_json_value_set (o, "format", bro_json_value_new_string (u->format));
  bro_json_value_set (o, "formatCode", bro_json_value_new_string (u->format_code));
  bro_json_value_set (o, "encrypted", bro_json_value_new_bool (u->encrypted));
  bro_json_value_set (o, "fingerprint", commit_s (u->fingerprint));
  bro_json_value_set (o, "label", bro_json_value_new_string (u->label));
  bro_json_value_set (o, "season", commit_n (u->has_season, u->season));
  bro_json_value_set (o, "part", commit_n (u->has_part, u->part));
  bro_json_value_set (o, "volume", commit_n (u->has_volume, u->volume));
  bro_json_value_set (o, "disc", commit_n (u->has_disc, u->disc));
  bro_json_value_set (o, "makemkvVersion", bro_json_value_new_string (u->makemkv_version));
  bro_json_value_set (o, "bytes", bro_json_value_new_integer (u->bytes));
  bro_json_value_set (o, "fileCount", bro_json_value_new_integer (u->file_count));
  bro_json_value_set (o, "attempts", bro_json_value_new_integer (u->attempts));
  bro_json_value_set (o, "createdAt", commit_instant (u->created_at));
  bro_json_value_set (o, "committedAt", u->has_committed_at ? commit_instant (u->committed_at) : bro_json_value_new_null ());
  return o;
}

static BroJsonValue *
commit_intent_json (const BroCommitIntent *i)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *items = bro_json_value_new_array ();
  for (guint k = 0; k < i->items->len; k++)
    {
      const BroCommitItem *x = i->items->pdata[k];
      BroJsonValue *item = bro_json_value_new_array ();
      bro_json_value_append (item, bro_json_value_new_integer (x->seq));
      bro_json_value_append (item, bro_json_value_new_string (x->from_path));
      bro_json_value_append (item, bro_json_value_new_string (x->to_path));
      bro_json_value_append (item, commit_s (x->sha256));
      bro_json_value_append (item, bro_json_value_new_bool (x->is_dir));
      bro_json_value_append (item, bro_json_value_new_bool (x->moved));
      bro_json_value_append (items, item);
    }
  bro_json_value_set (o, "unitId", bro_json_value_new_string (i->unit_id.value));
  bro_json_value_set (o, "jobId", bro_json_value_new_string (i->job_id.value));
  bro_json_value_set (o, "staging", bro_json_value_new_string (i->staging));
  bro_json_value_set (o, "destination", bro_json_value_new_string (i->destination));
  bro_json_value_set (o, "merge", bro_json_value_new_bool (i->merge));
  bro_json_value_set (o, "quarantine", bro_json_value_new_bool (i->quarantine));
  bro_json_value_set (o, "createdAt", commit_instant (i->created_at));
  bro_json_value_set (o, "items", items);
  return o;
}

/* A recorded commit or quarantine call (test-keep-apart.inc records its quarantines the same way). */
static BroJsonValue *
commit_call_json (const char *op, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, const char *tag)
{
  BroJsonValue *call = bro_json_value_new_object ();
  BroJsonValue *rows = bro_json_value_new_array ();
  for (guint i = 0; i < files->len; i++)
    {
      const BroUnitFile *f = files->pdata[i];
      BroJsonValue *row = bro_json_value_new_array ();
      bro_json_value_append (row, bro_json_value_new_string (f->path));
      bro_json_value_append (row, bro_json_value_new_integer (f->size));
      bro_json_value_append (row, bro_json_value_new_string (f->sha256));
      bro_json_value_append (row, bro_json_value_new_string (f->role));
      bro_json_value_append (row, commit_n (f->has_title, f->title));
      bro_json_value_append (rows, row);
    }
  bro_json_value_set (call, "op", bro_json_value_new_string (op));
  if (tag)
    bro_json_value_set (call, "tag", bro_json_value_new_string (tag));
  bro_json_value_set (call, "unit", commit_unit_json (unit));
  bro_json_value_set (call, "intent", commit_intent_json (intent));
  bro_json_value_set (call, "files", rows);
  return call;
}

static BroCommitResult *
commit_port_called (TestCommitPort *p, const char *op, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, const char *tag,
                    BroCancellationToken *cancel, BroBroError **error)
{
  BroJsonValue *call = commit_call_json (op, unit, intent, files, tag);
  BroJsonValue *port = bro_json_value_member (p->given, "port");
  BroJsonValue *why = bro_json_value_member (port, "error");
  const char *slash = strrchr (intent->destination, '/');
  g_autofree char *destination = NULL;
  GPtrArray *copy;
  g_mutex_lock (&p->lock);
  bro_json_value_append (p->calls, call);
  g_mutex_unlock (&p->lock);
  if (p->cancel_on_call)
    bro_cancellation_source_cancel (p->cancel_on_call);
  /* LibraryService ends its wait for the unit so when the token it was given is cancelled. */
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return NULL;
    }
  if (why)
    {
      *error = commit_error_of (why);
      return NULL;
    }
  if (STEPS_JS (port, "destination"))
    destination = g_strdup (STEPS_JS (port, "destination"));
  else if (tag)
    destination = g_strdup_printf ("%.*s/%s [%s]", (int) (slash - intent->destination), intent->destination, slash + 1, tag);
  else
    destination = g_strdup (intent->destination);
  copy = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (copy, bro_unit_file_copy (files->pdata[i]));
  return bro_commit_result_new (unit->id, destination, copy,
                                bro_json_value_get_bool (bro_json_value_member (port, "merged"), !tag && intent->merge));
}

static BroCommitResult *
commit_port_commit (BroLibraryPort *port, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, BroCancellationToken *cancel,
                    BroBroError **error)
{
  return commit_port_called (TEST_COMMIT_PORT (port), "commit", unit, intent, files, NULL, cancel, error);
}

static BroCommitResult *
commit_port_quarantine (BroLibraryPort *port, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, const char *tag,
                        BroCancellationToken *cancel, BroBroError **error)
{
  return commit_port_called (TEST_COMMIT_PORT (port), "quarantine", unit, intent, files, tag, cancel, error);
}

static void
commit_port_iface (BroLibraryPortInterface *i)
{
  i->require = commit_port_require;
  i->staging_directory = commit_port_staging_directory;
  i->commit = commit_port_commit;
  i->quarantine = commit_port_quarantine;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (TestCommitPort, test_commit_port, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_LIBRARY_PORT, commit_port_iface))

static void
test_commit_port_finalize (GObject *object)
{
  TestCommitPort *p = TEST_COMMIT_PORT (object);
  g_clear_pointer (&p->given, bro_json_value_unref);
  bro_json_value_unref (p->calls);
  g_mutex_clear (&p->lock);
  G_OBJECT_CLASS (test_commit_port_parent_class)->finalize (object);
}

static void
test_commit_port_class_init (TestCommitPortClass *k)
{
  G_OBJECT_CLASS (k)->finalize = test_commit_port_finalize;
}

static void
test_commit_port_init (TestCommitPort *p)
{
  g_mutex_init (&p->lock);
  p->calls = bro_json_value_new_array ();
}

/* ---- the cases ---- */

#define COMMIT_JOB_ID "00000000-0000-4000-8000-000000000001"

static gboolean
commit_steps_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroStep) step = g_strcmp0 (STEPS_JS (given, "step"), "notify") == 0 ? BRO_STEP (bro_notify_step_new ()) : BRO_STEP (bro_commit_step_new ());
  TestStepFakes *fakes = g_object_new (TEST_TYPE_STEP_FAKES, NULL);
  TestCommitPort *port = g_object_new (TEST_TYPE_COMMIT_PORT, NULL);
  TestSealClock *clock = g_object_new (TEST_TYPE_SEAL_CLOCK, NULL);
  TestSealRenderer *renderer = g_object_new (TEST_TYPE_SEAL_RENDERER, NULL);
  BroId job_id = { COMMIT_JOB_ID };
  g_autofree char *directory = g_strconcat ("/data/jobs/", COMMIT_JOB_ID, NULL);
  g_autoptr (BroJsonValue) default_config = bro_json_value_new_object ();
  g_autofree char *config_text = NULL;
  g_autoptr (BroConfig) config = NULL;
  g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
  g_autoptr (BroStepEnvironment) environment = NULL;
  g_autoptr (BroStepContext) context = NULL;
  g_autoptr (BroResourceSet) resources = NULL;
  g_autoptr (BroStepReport) report = NULL;
  g_autoptr (BroBroError) thrown = NULL;
  BroJobSnapshot *job;
  BroJsonValue *v, *entries;
  const BroBroError *error;
  const char *cancel_at = STEPS_JS (bro_json_value_member (given, "cancelAt"), "op");

  fakes->given = bro_json_value_ref (given);
  fakes->cancel = cancel;
  port->given = bro_json_value_ref (given);
  if (!bro_instant_parse (STEPS_JS (given, "now"), &clock->now))
    {
      bro_test_fail (failures, id, "given.now isn't an instant");
      goto out;
    }
  if (g_strcmp0 (cancel_at, "start") == 0)
    bro_cancellation_source_cancel (cancel);
  else if (g_strcmp0 (cancel_at, "commit") == 0)
    port->cancel_on_call = cancel;

  job = bro_job_snapshot_new (&job_id, steps_request_of (bro_json_value_member (given, "request")), NULL);
  entries = bro_json_value_member (given, "outputs");
  for (guint i = 0; i < bro_json_value_length (entries); i++)
    {
      BroJsonValue *o = bro_json_value_at (entries, i);
      BroStepKind kind;
      BroStepOutput *output;
      if (!bro_step_kind_from_wire (STEPS_JS (o, "kind"), &kind))
        {
          bro_test_fail (failures, id, "unknown step kind %s", STEPS_JS (o, "kind"));
          bro_job_snapshot_free (job);
          goto out;
        }
      output = bro_step_output_new (kind, bro_json_value_ref (bro_json_value_member (o, "data")), NULL);
      output->read_errors = (int) bro_json_value_get_integer (bro_json_value_member (o, "readErrors"), 0);
      g_ptr_array_add (job->outputs, output);
    }
  resources = bro_step_resources (step, job);

  bro_json_value_set (default_config, "version", bro_json_value_new_integer (3));
  config_text = bro_json_value_to_text (default_config);
  config = bro_config_codec_decode (config_text, -1, NULL);
  environment = bro_step_environment_new (NULL, BRO_FILE_SYSTEM (fakes), BRO_CLOCK (clock), NULL, BRO_LIBRARY_PORT (port), NULL, NULL, NULL, NULL,
                                          "/data", BRO_MESSAGE_RENDERER (renderer), "test");
  context = bro_step_context_new (job, bro_cancellation_source_token (cancel), g_object_ref (BRO_PROGRESS_REPORTER (fakes)),
                                  g_object_ref (BRO_JOB_LOGGER (fakes)), g_object_ref (BRO_DECISION_PORT (fakes)),
                                  g_object_ref (BRO_SPACE_RESERVATIONS (fakes)), g_object_ref (BRO_CHECKPOINTER (fakes)), environment, config,
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

  report = bro_step_run (step, context, &thrown);

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
  if (bro_json_value_get_bool (bro_json_value_member (expect, "quarantined"), FALSE) != (report && report->quarantined))
    bro_test_fail (failures, id, "quarantined: expected %s",
                   bro_json_value_get_bool (bro_json_value_member (expect, "quarantined"), FALSE) ? "true" : "false");
  if ((v = bro_json_value_member (expect, "data")))
    bro_test_same_json (failures, id, "data", v, report ? report->data : NULL);
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
            g_autofree char *what_severity = g_strdup_printf ("log[%u] severity", i);
            bro_test_same_string (failures, id, what, STEPS_JS (e, "code"), bro_message_code_wire (got->code));
            bro_test_same_json (failures, id, what_params, params ? params : none, got->params);
            bro_test_same_string (failures, id, what_severity, STEPS_JS (e, "severity"), bro_severity_to_wire (got->severity));
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
  if ((v = bro_json_value_member (expect, "calls")))
    bro_test_same_json (failures, id, "calls", v, port->calls);
  if ((v = bro_json_value_member (expect, "stagingAsked")))
    {
      gint64 want = bro_json_value_get_integer (v, -1);
      if (want != port->staging_asked)
        bro_test_fail (failures, id, "stagingAsked: expected %" G_GINT64_FORMAT ", got %d", want, port->staging_asked);
    }
out:
  g_clear_pointer (&context, bro_step_context_free);
  g_clear_pointer (&environment, bro_step_environment_free);
  g_object_unref (renderer);
  g_object_unref (clock);
  g_object_unref (port);
  g_object_unref (fakes);
  return TRUE;
}

static void
test_commit_steps (void)
{
  bro_test_run_cases ("engine/commit-steps.cases.json", commit_steps_case);
}
