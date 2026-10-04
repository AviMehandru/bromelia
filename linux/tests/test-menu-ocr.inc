/* test-menu-ocr.inc: BroMenuOcr, included by test-adapters.c: shared/fixtures/adapters/menu-ocr.cases.json against a
 * scripted launcher, and the real ffmpeg and tesseract where they are installed (the Ubuntu VM). */

static int
by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static char *
ocr_r (const char *text, const char *dir)
{
  g_autoptr (GString) s = g_string_new (text ? text : "");
  g_string_replace (s, "<dir>", dir, 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static gboolean
menu_ocr_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestScriptedLauncher) launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  g_autoptr (TestMapLocator) locator = g_object_new (TEST_TYPE_MAP_LOCATOR, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  g_autoptr (BroMenuOcr) ocr = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GStrvBuilder) lines = g_strv_builder_new ();
  g_autofree char *dir = g_build_filename (fs_root, "ocr", NULL);
  BroJsonValue *want_error = bro_json_value_member (expect, "error"), *located = bro_json_value_member (given, "located"), *v;
  BroJsonValue *write = bro_json_value_member (given, "writeArgument");
  remove_tree (fs_root);
  g_mkdir_with_parents (dir, 0755);
  for (guint i = 0; i < bro_json_value_length (bro_json_value_member (given, "lines")); i++)
    g_strv_builder_add (lines, bro_json_value_get_string (bro_json_value_at (bro_json_value_member (given, "lines"), i), ""));
  launcher->lines = g_strv_builder_end (lines);
  launcher->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (given, "exitCode"), 0);
  if (write)
    {
      launcher->write_index = (int) bro_json_value_get_integer (bro_json_value_member (write, "index"), -1);
      launcher->write_text = g_strdup (bro_json_value_get_string (bro_json_value_member (write, "text"), ""));
    }
  for (guint i = 0; i < bro_json_value_length (located); i++)
    {
      const char *name = bro_json_value_get_string (bro_json_value_at (located, i), "");
      BroToolKind kind = g_str_equal (name, "ffmpeg") ? BRO_TOOL_KIND_FFMPEG : BRO_TOOL_KIND_TESSERACT;
      g_hash_table_insert (locator->paths, GINT_TO_POINTER (kind), g_strconcat ("/opt/", name, NULL));
    }
  if (bro_json_value_get_bool (bro_json_value_member (given, "cancelled"), FALSE))
    bro_cancellation_token_cancel (cancel);
  ocr = bro_menu_ocr_new (BRO_PROCESS_LAUNCHER (launcher), BRO_TOOL_LOCATOR (locator), BRO_FILE_SYSTEM (fs));

  if (g_str_equal (bro_json_value_get_string (bro_json_value_member (given, "op"), ""), "extractStills"))
    {
      g_autofree char *iso = bro_test_fixture_path ("adapters/video-ts.iso");
      g_autoptr (BroVideoTsByteSource) source = bro_video_ts_byte_source_open (iso);
      g_autoptr (GPtrArray) cells = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_cell_ref_free);
      g_auto (GStrv) stills = NULL;
      BroJsonValue *given_cells = bro_json_value_member (given, "cells");
      for (guint i = 0; i < bro_json_value_length (given_cells); i++)
        {
          BroJsonValue *c = bro_json_value_at (given_cells, i);
          g_ptr_array_add (cells, bro_cell_ref_new (bro_json_value_get_string (bro_json_value_at (c, 0), ""), bro_json_value_get_integer (bro_json_value_at (c, 1), 0),
                                                    bro_json_value_get_integer (bro_json_value_at (c, 2), 0)));
        }
      stills = bro_menu_ocr_extract_stills (ocr, (BroByteSource *) source, cells, dir, cancel, &error);
      if (stills)
        {
          g_autoptr (GPtrArray) got = g_ptr_array_new ();
          g_autoptr (GPtrArray) want = g_ptr_array_new_with_free_func (g_free);
          g_autofree char *want_text = NULL, *got_text = NULL;
          for (guint i = 0; stills[i]; i++)
            g_ptr_array_add (got, stills[i]);
          for (guint i = 0; i < bro_json_value_length (bro_json_value_member (expect, "stills")); i++)
            g_ptr_array_add (want, ocr_r (bro_json_value_get_string (bro_json_value_at (bro_json_value_member (expect, "stills"), i), ""), dir));
          want_text = hb_joined (want);
          got_text = hb_joined (got);
          bro_test_same_string (failures, id, "stills", want_text, got_text);
        }
    }
  else
    {
      g_autoptr (GPtrArray) stills = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GArray) numbers = NULL;
      for (guint i = 0; i < bro_json_value_length (bro_json_value_member (given, "stills")); i++)
        g_ptr_array_add (stills, ocr_r (bro_json_value_get_string (bro_json_value_at (bro_json_value_member (given, "stills"), i), ""), dir));
      g_ptr_array_add (stills, NULL);
      numbers = bro_menu_ocr_read_numbers (ocr, (const char *const *) stills->pdata, cancel, &error);
      if (numbers)
        {
          g_autoptr (GString) want = g_string_new (NULL), got = g_string_new (NULL);
          for (guint i = 0; i < bro_json_value_length (bro_json_value_member (expect, "numbers")); i++)
            g_string_append_printf (want, "%" G_GINT64_FORMAT ",", bro_json_value_get_integer (bro_json_value_at (bro_json_value_member (expect, "numbers"), i), -1));
          for (guint i = 0; i < numbers->len; i++)
            g_string_append_printf (got, "%d,", g_array_index (numbers, int, i));
          bro_test_same_string (failures, id, "numbers", want->str, got->str);
        }
    }
  if (error)
    {
      bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (want_error, "code"), NULL), error->code);
      for (guint i = 0; want_error && i < bro_json_value_length (bro_json_value_member (want_error, "params")); i++)
        {
          BroJsonValue *params = bro_json_value_member (want_error, "params");
          bro_test_same_json (failures, id, params->keys->pdata[i], params->items->pdata[i], bro_json_value_member (error->params, params->keys->pdata[i]));
        }
    }
  else if (want_error)
    bro_test_fail (failures, id, "expected an error");
  if ((v = bro_json_value_member (expect, "started")) && (gint64) launcher->started->len != bro_json_value_get_integer (v, -1))
    bro_test_fail (failures, id, "started %u", launcher->started->len);
  if ((v = bro_json_value_member (expect, "argv")))
    {
      g_autoptr (GString) want = g_string_new (NULL), got = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        {
          g_autofree char *line = hb_expected (bro_json_value_at (v, i), FALSE);
          g_autofree char *line_r = ocr_r (line, dir);
          g_string_append_printf (want, "%s\n", line_r);
        }
      for (guint i = 0; i < launcher->started->len; i++)
        {
          BroProcessSpec *spec = launcher->started->pdata[i];
          g_string_append (got, spec->executable);
          for (guint k = 0; spec->arguments[k]; k++)
            g_string_append_printf (got, "|%s", spec->arguments[k]);
          g_string_append_c (got, '\n');
          if (spec->stall_timeout.seconds != bro_json_value_get_number (bro_json_value_member (expect, "stallSeconds"), -1))
            bro_test_fail (failures, id, "stall timeout");
        }
      bro_test_same_string (failures, id, "argv", want->str, got->str);
    }
  if ((v = bro_json_value_member (expect, "left")))
    {
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *want = hb_expected (v, FALSE), *got = NULL;
      GDir *d = g_dir_open (dir, 0, NULL);
      const char *name;
      while (d && (name = g_dir_read_name (d)) != NULL)
        g_ptr_array_add (names, g_strdup (name));
      if (d)
        g_dir_close (d);
      g_ptr_array_sort (names, (GCompareFunc) by_text);
      got = hb_joined (names);
      bro_test_same_string (failures, id, "left", want, got);
    }
  return TRUE;
}

static void
test_menu_ocr (void)
{
  bro_test_run_cases ("adapters/menu-ocr.cases.json", menu_ocr_case);
}

/* The real tools (where installed): a menu still with two episode numbers drawn on it, inside a VIDEO_TS folder. */
static void
test_menu_ocr_real (void)
{
  g_autofree char *ffmpeg = g_find_program_in_path ("ffmpeg"), *tesseract = g_find_program_in_path ("tesseract");
  const char *font = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
  g_autofree char *disc = NULL, *video_ts = NULL, *vob = NULL, *ifo_src = NULL, *ifo = NULL, *work = NULL, *ifo_text = NULL, *filter = NULL;
  g_autoptr (BroPlatformProcessLauncher) launcher = NULL;
  g_autoptr (TestMapLocator) locator = NULL;
  g_autoptr (BroPlatformFileSystem) fs = NULL;
  g_autoptr (BroMenuOcr) ocr = NULL;
  g_autoptr (BroVideoTsByteSource) source = NULL;
  g_autoptr (GPtrArray) cells = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_auto (GStrv) stills = NULL;
  g_autoptr (GArray) numbers = NULL;
  gsize ifo_len = 0;
  gint64 sectors;
  struct stat st;
  int status;
  if (!ffmpeg || !tesseract || !g_file_test (font, G_FILE_TEST_EXISTS))
    {
      g_test_skip ("needs ffmpeg, tesseract and the DejaVu fonts");
      return;
    }
  disc = g_build_filename (scratch, "ocr-real", NULL);
  video_ts = g_build_filename (disc, "VIDEO_TS", NULL);
  work = g_build_filename (scratch, "ocr-real-work", NULL);
  g_mkdir_with_parents (video_ts, 0755);
  g_mkdir_with_parents (work, 0755);
  vob = g_build_filename (video_ts, "VIDEO_TS.VOB", NULL);
  ifo = g_build_filename (video_ts, "VIDEO_TS.IFO", NULL);
  ifo_src = bro_test_fixture_path ("dvd-play-all/VIDEO_TS/VIDEO_TS.IFO");
  g_assert_true (g_file_get_contents (ifo_src, &ifo_text, &ifo_len, NULL));
  g_assert_true (g_file_set_contents (ifo, ifo_text, (gssize) ifo_len, NULL));
  filter = g_strdup_printf ("drawtext=fontfile=%s:text='EPISODE 12':fontsize=48:x=60:y=120:fontcolor=white,"
                            "drawtext=fontfile=%s:text='EPISODE 13':fontsize=48:x=60:y=280:fontcolor=white", font, font);
  {
    const char *argv[] = { ffmpeg, "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=navy:s=720x480:d=1", "-vf", filter, "-frames:v", "1",
                           "-c:v", "mpeg2video", "-f", "vob", vob, NULL };
    g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, NULL));
    g_assert_cmpint (status, ==, 0);
  }
  g_assert_cmpint (stat (vob, &st), ==, 0);
  sectors = (st.st_size + 2047) / 2048;
  source = bro_video_ts_byte_source_open (disc);
  g_assert_nonnull (source);
  cells = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_cell_ref_free);
  g_ptr_array_add (cells, bro_cell_ref_new ("VIDEO_TS.VOB", 0, sectors));
  launcher = bro_platform_process_launcher_new ();
  locator = g_object_new (TEST_TYPE_MAP_LOCATOR, NULL);
  g_hash_table_insert (locator->paths, GINT_TO_POINTER (BRO_TOOL_KIND_FFMPEG), g_strdup (ffmpeg));
  g_hash_table_insert (locator->paths, GINT_TO_POINTER (BRO_TOOL_KIND_TESSERACT), g_strdup (tesseract));
  fs = bro_platform_file_system_new ();
  ocr = bro_menu_ocr_new (BRO_PROCESS_LAUNCHER (launcher), BRO_TOOL_LOCATOR (locator), BRO_FILE_SYSTEM (fs));
  stills = bro_menu_ocr_extract_stills (ocr, (BroByteSource *) source, cells, work, NULL, &error);
  g_assert_null (error);
  g_assert_cmpuint (g_strv_length (stills), ==, 1);
  numbers = bro_menu_ocr_read_numbers (ocr, (const char *const *) stills, NULL, &error);
  g_assert_null (error);
  g_assert_cmpuint (numbers->len, ==, 2);
  g_assert_cmpint (g_array_index (numbers, int, 0), ==, 12);
  g_assert_cmpint (g_array_index (numbers, int, 1), ==, 13);
  remove_tree (disc);
  remove_tree (work);
}
