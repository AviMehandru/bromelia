/* test-keep-apart.inc: shared/fixtures/engine/keep-apart-steps.cases.json: BroKeepApartStep alone, run by test-seal.inc's
 * harness, with a library port and checkpointer of its own: the port takes require and stagingDirectory from
 * test-steps.inc's fakes and records each quarantine as test-commit.inc's port does; the checkpointer holds what is saved
 * (given.checkpoint at first). The format is in claudeDocs/tools/rebuild-phase3/gen-keep-apart-cases.py. Included by
 * test-engine.c after test-commit.inc. */

#include "bro-keep-apart-step.h"

#define TEST_TYPE_KEEP_APART_PORT (test_keep_apart_port_get_type ())
G_DECLARE_FINAL_TYPE (TestKeepApartPort, test_keep_apart_port, TEST, KEEP_APART_PORT, GObject)

struct _TestKeepApartPort {
  GObject parent;
  GMutex lock;
  TestStepFakes *fakes;                        /* not owned */
  BroCancellationSource *cancel_on_quarantine; /* the job's, when cancelAt.op is "quarantine"; not owned; nullable */
  BroJsonValue *calls;                         /* array */
  BroJsonValue *held;                          /* what the checkpointer holds; nullable */
  BroJsonValue *saved;                         /* what it held when the run started; nullable */
  int saves;
};

static BroLibraryHandle *
keep_apart_port_require (BroLibraryPort *port, const char *library_id, BroBroError **error)
{
  return bro_library_port_require (BRO_LIBRARY_PORT (TEST_KEEP_APART_PORT (port)->fakes), library_id, error);
}

static char *
keep_apart_port_staging_directory (BroLibraryPort *port, const char *library_id, const BroId *job_id, BroBroError **error)
{
  return bro_library_port_staging_directory (BRO_LIBRARY_PORT (TEST_KEEP_APART_PORT (port)->fakes), library_id, job_id, error);
}

static BroCommitResult *
keep_apart_port_commit (BroLibraryPort *port, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, BroCancellationToken *cancel,
                        BroBroError **error)
{
  bro_bro_error_set (error, "internal.unexpected", NULL); /* never called */
  return NULL;
}

static BroCommitResult *
keep_apart_port_quarantine (BroLibraryPort *port, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, const char *tag,
                            BroCancellationToken *cancel, BroBroError **error)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (port);
  BroJsonValue *given = bro_json_value_member (p->fakes->given, "port");
  BroJsonValue *why = bro_json_value_member (given, "error");
  g_autofree char *destination = NULL;
  GPtrArray *copy;
  g_mutex_lock (&p->lock);
  bro_json_value_append (p->calls, commit_call_json ("quarantine", unit, intent, files, tag));
  g_mutex_unlock (&p->lock);
  if (p->cancel_on_quarantine)
    bro_cancellation_source_cancel (p->cancel_on_quarantine);
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
  destination = STEPS_JS (given, "destination") ? g_strdup (STEPS_JS (given, "destination")) : g_strdup_printf ("%s [%s]", intent->destination, tag);
  copy = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (copy, bro_unit_file_copy (files->pdata[i]));
  return bro_commit_result_new (unit->id, destination, copy, FALSE);
}

static void
keep_apart_port_iface (BroLibraryPortInterface *i)
{
  i->require = keep_apart_port_require;
  i->staging_directory = keep_apart_port_staging_directory;
  i->commit = keep_apart_port_commit;
  i->quarantine = keep_apart_port_quarantine;
}

static BroJsonValue *
keep_apart_port_saved (BroCheckpointer *checkpointer)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (checkpointer);
  BroJsonValue *saved;
  g_mutex_lock (&p->lock);
  saved = p->saved ? bro_json_value_ref (p->saved) : NULL;
  g_mutex_unlock (&p->lock);
  return saved;
}

static gboolean
keep_apart_port_save (BroCheckpointer *checkpointer, const BroJsonValue *checkpoint, BroBroError **error)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (checkpointer);
  g_mutex_lock (&p->lock);
  g_clear_pointer (&p->held, bro_json_value_unref);
  p->held = bro_json_value_ref ((BroJsonValue *) checkpoint);
  p->saves++;
  g_mutex_unlock (&p->lock);
  return TRUE;
}

static void
keep_apart_port_checkpointer_iface (BroCheckpointerInterface *i)
{
  i->saved = keep_apart_port_saved;
  i->save = keep_apart_port_save;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (TestKeepApartPort, test_keep_apart_port, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_LIBRARY_PORT, keep_apart_port_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CHECKPOINTER, keep_apart_port_checkpointer_iface))

static void
test_keep_apart_port_finalize (GObject *object)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (object);
  bro_json_value_unref (p->calls);
  g_clear_pointer (&p->held, bro_json_value_unref);
  g_clear_pointer (&p->saved, bro_json_value_unref);
  g_mutex_clear (&p->lock);
  G_OBJECT_CLASS (test_keep_apart_port_parent_class)->finalize (object);
}

static void
test_keep_apart_port_class_init (TestKeepApartPortClass *k)
{
  G_OBJECT_CLASS (k)->finalize = test_keep_apart_port_finalize;
}

static void
test_keep_apart_port_init (TestKeepApartPort *p)
{
  g_mutex_init (&p->lock);
  p->calls = bro_json_value_new_array ();
}

static void
keep_apart_setup (GObject *port, TestStepFakes *fakes, BroCancellationSource *cancel)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (port);
  BroJsonValue *held = bro_json_value_member (fakes->given, "checkpoint");
  p->fakes = fakes;
  if (g_strcmp0 (STEPS_JS (bro_json_value_member (fakes->given, "cancelAt"), "op"), "quarantine") == 0)
    p->cancel_on_quarantine = cancel;
  if (held && held->kind != BRO_JSON_VALUE_NULL)
    p->held = bro_json_value_ref (held);
}

static void
keep_apart_start_run (GObject *port, gboolean last)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (port);
  g_mutex_lock (&p->lock);
  g_clear_pointer (&p->saved, bro_json_value_unref);
  p->saved = p->held ? bro_json_value_ref (p->held) : NULL;
  if (last)
    p->saves = 0;
  g_mutex_unlock (&p->lock);
}

static void
keep_apart_check (GObject *port, const char *id, BroJsonValue *expect, GPtrArray *failures)
{
  TestKeepApartPort *p = TEST_KEEP_APART_PORT (port);
  BroJsonValue *v;
  if ((v = bro_json_value_member (expect, "calls")))
    bro_test_same_json (failures, id, "calls", v, p->calls);
  for (guint i = 0; i < expect->keys->len; i++)
    if (strcmp (expect->keys->pdata[i], "checkpoint") == 0)
      {
        g_autoptr (BroJsonValue) none = bro_json_value_new_null ();
        bro_test_same_json (failures, id, "checkpoint", expect->items->pdata[i], p->held ? p->held : none);
      }
  if ((v = bro_json_value_member (expect, "saves")) && bro_json_value_get_integer (v, -1) != p->saves)
    bro_test_fail (failures, id, "saves: expected %" G_GINT64_FORMAT ", got %d", bro_json_value_get_integer (v, -1), p->saves);
}

static gboolean
keep_apart_steps_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroStep) step = BRO_STEP (bro_keep_apart_step_new ());
  g_autoptr (TestKeepApartPort) port = g_object_new (TEST_TYPE_KEEP_APART_PORT, NULL);
  SealHooks hooks = { step, G_OBJECT (port), keep_apart_setup, keep_apart_start_run, keep_apart_check };
  return seal_case_run (id, given, expect, failures, &hooks);
}

static void
test_keep_apart_steps (void)
{
  bro_test_run_cases ("engine/keep-apart-steps.cases.json", keep_apart_steps_case);
}
