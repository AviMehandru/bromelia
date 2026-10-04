/* test-mkv.inc: MkvToolNix, included by test-adapters.c: shared/fixtures/adapters/mkvtoolnix.cases.json. */

static char *
with_fs_root (const char *text)
{
  g_autoptr (GString) s = g_string_new (text);
  g_string_replace (s, "<root>", fs_root, 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static char *
joined_shown (const char *const *args, int from, int count)
{
  g_autoptr (GString) out = g_string_new (NULL);
  int n = (int) g_strv_length ((char **) args);
  if (from < 0)
    from = n + from;
  for (int i = from; i < n && (count < 0 || i < from + count); i++)
    {
      g_autofree char *shown = shown_text (args[i]);
      g_autoptr (GString) s = g_string_new (shown);
      g_string_replace (s, "<work>", "<root>/home", 0);
      g_string_append_printf (out, "%s%s", out->len ? "|" : "", s->str);
    }
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static int
count_prefixed (const char *prefix)
{
  GDir *d = g_dir_open (fs_root, 0, NULL);
  const char *name;
  int n = 0;
  while (d && (name = g_dir_read_name (d)) != NULL)
    if (g_str_has_prefix (name, prefix))
      n++;
  if (d)
    g_dir_close (d);
  return n;
}

static gboolean
mkv_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestScriptedLauncher) launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  g_autoptr (TestFixedLocator) locator = g_object_new (TEST_TYPE_FIXED_LOCATOR, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroMkvToolNix) mkv = NULL;
  g_autoptr (BroBroError) error = NULL;
  BroJsonValue *call = bro_json_value_member (given, "call");
  BroJsonValue *write = bro_json_value_member (given, "writeArgument");
  BroJsonValue *extract = bro_json_value_member (given, "mkvextract");
  BroJsonValue *v;
  const char *what = bro_json_value_get_string (bro_json_value_at (call, 0), "");
  BroProcessSpec *spec;
  remove_tree (fs_root);
  g_mkdir_with_parents (fs_root, 0755);
  launcher->lines = case_lines (given);
  launcher->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (given, "exitCode"), 0);
  launcher->split_parts = (int) bro_json_value_get_integer (bro_json_value_member (given, "splitParts"), 0);
  if (write)
    {
      launcher->write_index = (int) bro_json_value_get_integer (bro_json_value_member (write, "index"), -1);
      launcher->write_text = g_strdup (bro_json_value_get_string (bro_json_value_member (write, "text"), ""));
    }
  locator->mkvmerge = g_strdup ("/opt/mkvmerge");
  locator->mkvextract = extract && extract->kind == BRO_JSON_VALUE_NULL ? NULL : g_strdup ("/opt/mkvextract");
  mkv = bro_mkv_tool_nix_new (BRO_PROCESS_LAUNCHER (launcher), BRO_FILE_SYSTEM (fs), BRO_TOOL_LOCATOR (locator), fs_root);
  if (g_str_equal (what, "probe"))
    {
      g_autofree char *file = with_fs_root (bro_json_value_get_string (bro_json_value_at (call, 1), ""));
      g_autoptr (BroMkvProbe) probe = bro_mkv_tool_nix_probe (mkv, file, NULL, &error);
      BroJsonValue *want = bro_json_value_member (expect, "probe");
      if (want->kind == BRO_JSON_VALUE_NULL)
        {
          if (probe)
            bro_test_fail (failures, id, "a probe");
        }
      else if (!probe)
        bro_test_fail (failures, id, "no probe");
      else
        {
          if ((v = bro_json_value_member (want, "durationSeconds")) && bro_json_value_get_number (v, 0) != probe->duration_seconds)
            bro_test_fail (failures, id, "duration %f", probe->duration_seconds);
          if ((v = bro_json_value_member (want, "tracks")) && v->integer != (gint64) probe->tracks->len)
            bro_test_fail (failures, id, "tracks %u", probe->tracks->len);
          if ((v = bro_json_value_member (want, "chapterCount")) && v->integer != probe->chapter_count)
            bro_test_fail (failures, id, "chapters %d", probe->chapter_count);
        }
    }
  else if (g_str_equal (what, "remux"))
    {
      BroJsonValue *list = bro_json_value_at (call, 1);
      g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
      gboolean ok;
      for (guint i = 0; i < bro_json_value_length (list); i++)
        g_ptr_array_add (args, with_fs_root (bro_json_value_get_string (bro_json_value_at (list, i), "")));
      g_ptr_array_add (args, NULL);
      ok = bro_mkv_tool_nix_remux (mkv, (const char *const *) args->pdata, NULL, &error);
      if (ok != bro_json_value_get_bool (bro_json_value_member (expect, "ok"), !ok))
        bro_test_fail (failures, id, "ok %d", ok);
    }
  else if (g_str_equal (what, "split"))
    {
      BroJsonValue *list = bro_json_value_at (call, 1);
      g_autoptr (GArray) chapters = g_array_new (FALSE, FALSE, sizeof (int));
      g_autofree char *input = with_fs_root (bro_json_value_get_string (bro_json_value_at (call, 2), ""));
      g_auto (GStrv) parts = NULL;
      BroJsonValue *want = bro_json_value_member (expect, "parts");
      for (guint i = 0; i < bro_json_value_length (list); i++)
        {
          int c = (int) bro_json_value_get_integer (bro_json_value_at (list, i), 0);
          g_array_append_val (chapters, c);
        }
      parts = bro_mkv_tool_nix_split (mkv, chapters, input, NULL, &error);
      if (want->kind == BRO_JSON_VALUE_NULL ? parts != NULL : (!parts || (gint64) g_strv_length (parts) != want->integer))
        bro_test_fail (failures, id, "parts %u", parts ? g_strv_length (parts) : 0);
      if ((v = bro_json_value_member (expect, "leftOver")) && count_prefixed (".bromelia-split-") != v->integer)
        bro_test_fail (failures, id, "left over %d", count_prefixed (".bromelia-split-"));
    }
  else if (g_str_equal (what, "chapterTimes"))
    {
      g_autofree char *file = with_fs_root (bro_json_value_get_string (bro_json_value_at (call, 1), ""));
      g_autoptr (GArray) times = bro_mkv_tool_nix_chapter_times (mkv, file, NULL, &error);
      BroJsonValue *want = bro_json_value_member (expect, "times");
      if (want)
        {
          if (!times || times->len != bro_json_value_length (want))
            bro_test_fail (failures, id, "times");
          for (guint i = 0; times && i < times->len && i < bro_json_value_length (want); i++)
            if (g_array_index (times, BroDuration, i).seconds != bro_json_value_get_number (bro_json_value_at (want, i), -1))
              bro_test_fail (failures, id, "time %u", i);
        }
      if (count_prefixed (".bromelia-chapters-"))
        bro_test_fail (failures, id, "the chapter file is left");
    }
  else
    return FALSE;
  bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (expect, "error"), NULL), error ? error->code : NULL);
  spec = launcher->started->len ? launcher->started->pdata[0] : NULL;
  if ((v = bro_json_value_member (expect, "launched")) && !bro_json_value_get_bool (v, TRUE) && spec)
    bro_test_fail (failures, id, "launched");
  if ((v = bro_json_value_member (expect, "executable")))
    bro_test_same_string (failures, id, "executable", bro_json_value_get_string (v, ""), spec ? spec->executable : NULL);
  if ((v = bro_json_value_member (expect, "arguments")))
    {
      g_autofree char *want = joined_strings (v);
      g_autofree char *got = spec ? joined_shown ((const char *const *) spec->arguments, 0, -1) : NULL;
      bro_test_same_string (failures, id, "arguments", want, got);
    }
  if ((v = bro_json_value_member (expect, "argumentsStart")))
    {
      g_autofree char *want = joined_strings (v);
      g_autofree char *got = spec ? joined_shown ((const char *const *) spec->arguments, 0, (int) bro_json_value_length (v)) : NULL;
      bro_test_same_string (failures, id, "argumentsStart", want, got);
    }
  if ((v = bro_json_value_member (expect, "argumentsEnd")))
    {
      g_autofree char *want = joined_strings (v);
      g_autofree char *got = spec ? joined_shown ((const char *const *) spec->arguments, -(int) bro_json_value_length (v), -1) : NULL;
      bro_test_same_string (failures, id, "argumentsEnd", want, got);
    }
  return TRUE;
}

static void
test_mkvtoolnix (void)
{
  bro_test_run_cases ("adapters/mkvtoolnix.cases.json", mkv_case);
}
