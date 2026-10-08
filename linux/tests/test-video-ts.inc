/* test-video-ts.inc: BroVideoTsByteSource, included by test-adapters.c:
 * shared/fixtures/adapters/video-ts-byte-source.cases.json, and the real DVD ISO when BROMELIA_TEST_DVD_ISO is set. */

/* What bro_dvd_nav_analyse found, in short: each title's number and chapter count, and the jumps. */
static char *
nav_summary (BroByteSource *source)
{
  BroNavAnalysis *a = bro_dvd_nav_analyse (source);
  GString *s;
  if (!a)
    return g_strdup ("none");
  s = g_string_new (NULL);
  for (guint i = 0; i < a->titles->len; i++)
    {
      BroNavTitle *t = a->titles->pdata[i];
      g_string_append_printf (s, "%s%d:%u", i ? "," : "", t->number, t->chapters->len);
    }
  g_string_append (s, " / ");
  for (guint i = 0; i < a->jumps->len; i++)
    {
      BroNavJump *j = a->jumps->pdata[i];
      g_string_append_printf (s, "%s%d.%d %s", i ? "," : "", j->title, j->chapter, j->how);
    }
  bro_nav_analysis_free (a);
  return g_string_free (s, FALSE);
}

static gboolean
video_ts_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autofree char *path = bro_test_fixture_path (bro_json_value_get_string (bro_json_value_member (given, "open"), ""));
  g_autoptr (BroVideoTsByteSource) source = bro_video_ts_byte_source_open (path);
  BroJsonValue *files = bro_json_value_member (expect, "files"), *reads = bro_json_value_member (expect, "reads");
  const char *other = bro_json_value_get_string (bro_json_value_member (expect, "sameAnalysisAs"), NULL);
  if (!bro_json_value_get_bool (bro_json_value_member (expect, "opens"), TRUE))
    {
      if (source)
        bro_test_fail (failures, id, "opened");
      return TRUE;
    }
  if (!source)
    {
      bro_test_fail (failures, id, "not opened");
      return TRUE;
    }
  bro_test_same_string (failures, id, "label", bro_json_value_get_string (bro_json_value_member (expect, "label"), NULL),
                        bro_video_ts_byte_source_label (source));
  if (files)
    {
      g_autoptr (GPtrArray) got = bro_byte_source_files ((BroByteSource *) source);
      g_autoptr (GString) want_text = g_string_new (NULL), got_text = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (files); i++)
        g_string_append_printf (want_text, "%s %" G_GINT64_FORMAT ";", bro_json_value_get_string (bro_json_value_at (bro_json_value_at (files, i), 0), ""),
                                bro_json_value_get_integer (bro_json_value_at (bro_json_value_at (files, i), 1), -1));
      for (guint i = 0; i < got->len; i++)
        g_string_append_printf (got_text, "%s %" G_GINT64_FORMAT ";", ((BroByteFile *) got->pdata[i])->name, ((BroByteFile *) got->pdata[i])->size);
      bro_test_same_string (failures, id, "files", want_text->str, got_text->str);
    }
  for (guint i = 0; i < bro_json_value_length (reads); i++)
    {
      BroJsonValue *r = bro_json_value_at (reads, i);
      g_autoptr (GBytes) bytes = bro_byte_source_read ((BroByteSource *) source, bro_json_value_get_string (bro_json_value_member (r, "file"), ""),
                                                       bro_json_value_get_integer (bro_json_value_member (r, "offset"), 0),
                                                       (gsize) bro_json_value_get_integer (bro_json_value_member (r, "length"), 0));
      gsize n = 0;
      const guint8 *data = g_bytes_get_data (bytes, &n);
      const char *text = bro_json_value_get_string (bro_json_value_member (r, "text"), NULL);
      const char *hex = bro_json_value_get_string (bro_json_value_member (r, "hex"), NULL);
      BroJsonValue *count = bro_json_value_member (r, "count");
      if (text)
        {
          g_autofree char *got = g_strndup ((const char *) data, n);
          bro_test_same_string (failures, id, "text", text, got);
          if (n != strlen (text)) /* a NUL byte after the text would end the string compared above */
            bro_test_fail (failures, id, "text: %" G_GSIZE_FORMAT " bytes read, %" G_GSIZE_FORMAT " expected", (gsize) n, strlen (text));
        }
      if (hex)
        {
          g_autoptr (GString) got = g_string_new (NULL);
          for (gsize k = 0; k < n; k++)
            g_string_append_printf (got, "%02x", data[k]);
          bro_test_same_string (failures, id, "hex", hex, got->str);
        }
      if (count && (gint64) n != bro_json_value_get_integer (count, -1))
        bro_test_fail (failures, id, "count %" G_GSIZE_FORMAT, n);
    }
  if (other)
    {
      g_autofree char *other_path = bro_test_fixture_path (other);
      g_autoptr (BroVideoTsByteSource) folder = bro_video_ts_byte_source_open (other_path);
      g_autofree char *mine = nav_summary ((BroByteSource *) source), *theirs = folder ? nav_summary ((BroByteSource *) folder) : NULL;
      if (g_str_equal (mine, "none"))
        bro_test_fail (failures, id, "not analysed");
      bro_test_same_string (failures, id, "analysis", theirs, mine);
    }
  return TRUE;
}

static void
test_video_ts (void)
{
  bro_test_run_cases ("adapters/video-ts-byte-source.cases.json", video_ts_case);
}

static void
test_video_ts_real_iso (void)
{
  const char *iso = g_getenv ("BROMELIA_TEST_DVD_ISO");
  g_autoptr (BroVideoTsByteSource) source = NULL;
  g_autoptr (GBytes) head = NULL;
  g_autofree char *summary = NULL;
  if (!iso || !*iso)
    {
      g_test_skip ("BROMELIA_TEST_DVD_ISO isn't set");
      return;
    }
  source = bro_video_ts_byte_source_open (iso);
  g_assert_nonnull (source);
  head = bro_byte_source_read ((BroByteSource *) source, "VIDEO_TS.IFO", 0, 12);
  g_assert_cmpmem (g_bytes_get_data (head, NULL), g_bytes_get_size (head), "DVDVIDEO-VMG", 12);
  summary = nav_summary ((BroByteSource *) source);
  g_assert_cmpstr (summary, !=, "none");
  g_test_message ("%s: %s", bro_video_ts_byte_source_label (source), summary);
}
