/* test-cd-ripper.inc: BroCdRipper, included by test-adapters.c: shared/fixtures/adapters/cd-ripper.cases.json against a
 * scripted launcher. */

/* Finds the tools it is given (BroToolKind → path). */
#define TEST_TYPE_MAP_LOCATOR (test_map_locator_get_type ())
G_DECLARE_FINAL_TYPE (TestMapLocator, test_map_locator, TEST, MAP_LOCATOR, GObject)
struct _TestMapLocator {
  GObject parent_instance;
  GHashTable *paths;
};
static void test_map_locator_iface_init (BroToolLocatorInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestMapLocator, test_map_locator, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_TOOL_LOCATOR, test_map_locator_iface_init))

static BroToolInfo *
ml_locate (BroToolLocator *l, BroToolKind tool)
{
  const char *path = g_hash_table_lookup (TEST_MAP_LOCATOR (l)->paths, GINT_TO_POINTER (tool));
  BroToolInfo *info = bro_tool_info_new ();
  info->tool = tool;
  if (path)
    info->path = g_strdup (path);
  else
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (tool)));
      info->why = bro_bro_message_new (BRO_MSG_TOOL_MISSING, params, BRO_SEVERITY_WARNING);
    }
  return info;
}

static void test_map_locator_finalize (GObject *o) { g_hash_table_unref (TEST_MAP_LOCATOR (o)->paths); G_OBJECT_CLASS (test_map_locator_parent_class)->finalize (o); }
static void test_map_locator_class_init (TestMapLocatorClass *k) { G_OBJECT_CLASS (k)->finalize = test_map_locator_finalize; }
static void test_map_locator_init (TestMapLocator *self) { self->paths = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free); }
static void test_map_locator_iface_init (BroToolLocatorInterface *iface) { iface->locate = ml_locate; }

static char *
cd_r (const char *text)
{
  g_autoptr (GString) s = g_string_new (text ? text : "");
  g_string_replace (s, "<root>", fs_root, 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static gboolean
cd_ripper_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestScriptedLauncher) launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  g_autoptr (TestMapLocator) locator = g_object_new (TEST_TYPE_MAP_LOCATOR, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (TestRecordingSink) sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  g_autoptr (BroCdRipper) ripper = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GStrvBuilder) lines = g_strv_builder_new ();
  g_autofree char *dest = cd_r (bro_json_value_get_string (bro_json_value_member (given, "dest"), ""));
  g_auto (GStrv) files = NULL;
  BroJsonValue *want_error = bro_json_value_member (expect, "error"), *located = bro_json_value_member (given, "located"), *v;
  const char *saves = bro_json_value_get_string (bro_json_value_member (given, "saves"), NULL);
  int count = 0;
  remove_tree (fs_root);
  g_mkdir_with_parents (dest, 0755);
  launcher->stderr_at = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < bro_json_value_length (bro_json_value_member (given, "lines")); i++)
    {
      BroJsonValue *line = bro_json_value_at (bro_json_value_member (given, "lines"), i);
      const char *err = bro_json_value_get_string (bro_json_value_member (line, "stderr"), NULL);
      if (err)
        g_array_append_val (launcher->stderr_at, count);
      g_strv_builder_add (lines, err ? err : bro_json_value_get_string (line, ""));
      count++;
    }
  launcher->lines = g_strv_builder_end (lines);
  launcher->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (given, "exitCode"), 0);
  launcher->stalls = bro_json_value_get_bool (bro_json_value_member (given, "stalls"), FALSE);
  if (saves)
    {
      launcher->write_file_in = g_strdup (dest);
      launcher->write_files = g_strdupv ((char *[]) { (char *) saves, NULL });
    }
  for (guint i = 0; i < bro_json_value_length (located); i++)
    {
      const char *name = bro_json_value_get_string (bro_json_value_at (located, i), "");
      BroToolKind kind = g_str_equal (name, "cyanrip") ? BRO_TOOL_KIND_CYANRIP : BRO_TOOL_KIND_ABCDE;
      g_hash_table_insert (locator->paths, GINT_TO_POINTER (kind), g_strconcat ("/opt/", name, NULL));
    }
  if (bro_json_value_get_bool (bro_json_value_member (given, "cancelled"), FALSE))
    bro_cancellation_token_cancel (cancel);
  ripper = bro_cd_ripper_new (BRO_PROCESS_LAUNCHER (launcher), BRO_TOOL_LOCATOR (locator), BRO_FILE_SYSTEM (fs));
  files = bro_cd_ripper_rip (ripper, bro_json_value_get_string (bro_json_value_member (given, "device"), ""), dest,
                             bro_json_value_get_string (bro_json_value_member (given, "command"), NULL),
                             (int) bro_json_value_get_integer (bro_json_value_member (given, "stallMinutes"), 0), BRO_RUN_SINK (sink), cancel, &error);
  if (files)
    {
      g_autoptr (GPtrArray) got = g_ptr_array_new ();
      g_autofree char *want = NULL, *got_text = NULL;
      g_autoptr (GPtrArray) wanted = g_ptr_array_new_with_free_func (g_free);
      if (want_error)
        bro_test_fail (failures, id, "expected an error");
      for (guint i = 0; files[i]; i++)
        g_ptr_array_add (got, files[i]);
      for (guint i = 0; i < bro_json_value_length (bro_json_value_member (expect, "files")); i++)
        g_ptr_array_add (wanted, cd_r (bro_json_value_get_string (bro_json_value_at (bro_json_value_member (expect, "files"), i), "")));
      want = hb_joined (wanted);
      got_text = hb_joined (got);
      bro_test_same_string (failures, id, "files", want, got_text);
    }
  else if (!error)
    bro_test_fail (failures, id, "NULL without an error");
  if (error)
    {
      bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (want_error, "code"), NULL), error->code);
      for (guint i = 0; want_error && i < bro_json_value_length (bro_json_value_member (want_error, "params")); i++)
        {
          BroJsonValue *params = bro_json_value_member (want_error, "params");
          const char *key = params->keys->pdata[i];
          bro_test_same_json (failures, id, key, params->items->pdata[i], bro_json_value_member (error->params, key));
        }
    }
  if (!bro_json_value_get_bool (bro_json_value_member (expect, "started"), TRUE) && launcher->started->len)
    bro_test_fail (failures, id, "started");
  if ((v = bro_json_value_member (expect, "argv")))
    {
      g_autofree char *want = hb_expected (v, FALSE);
      g_autoptr (GPtrArray) got = g_ptr_array_new ();
      g_autofree char *got_text = NULL;
      if (launcher->started->len)
        {
          BroProcessSpec *spec = launcher->started->pdata[0];
          g_ptr_array_add (got, spec->executable);
          for (guint i = 0; spec->arguments[i]; i++)
            g_ptr_array_add (got, spec->arguments[i]);
        }
      got_text = hb_joined (got);
      bro_test_same_string (failures, id, "argv", want, got_text);
    }
  if ((v = bro_json_value_member (expect, "workingDirectory")))
    {
      g_autofree char *want = cd_r (bro_json_value_get_string (v, ""));
      bro_test_same_string (failures, id, "workingDirectory", want,
                            launcher->started->len ? ((BroProcessSpec *) launcher->started->pdata[0])->working_directory : NULL);
    }
  if ((v = bro_json_value_member (expect, "stallSeconds")) && launcher->started->len)
    {
      BroProcessSpec *spec = launcher->started->pdata[0];
      if (v->kind == BRO_JSON_VALUE_NULL ? spec->has_stall_timeout
                                         : (!spec->has_stall_timeout || spec->stall_timeout.seconds != bro_json_value_get_number (v, -1)))
        bro_test_fail (failures, id, "stall timeout");
    }
  if ((v = bro_json_value_member (expect, "raw")))
    {
      g_autofree char *want = hb_expected (v, FALSE), *got = hb_joined (sink->raw);
      bro_test_same_string (failures, id, "raw", want, got);
    }
  return TRUE;
}

static void
test_cd_ripper (void)
{
  bro_test_run_cases ("adapters/cd-ripper.cases.json", cd_ripper_case);
}
