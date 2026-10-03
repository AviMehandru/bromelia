/* bro-test-fixtures.c */
#include "bro-test-fixtures.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *
bro_test_fixture_path (const char *relative)
{
  const char *root = g_getenv ("BROMELIA_FIXTURES");
  g_assert_nonnull (root);
  return g_build_filename (root, relative, NULL);
}

char *
bro_test_fixture_text (const char *relative, gsize *length)
{
  g_autofree char *path = bro_test_fixture_path (relative);
  char *text = NULL;
  g_autoptr (GError) error = NULL;
  if (!g_file_get_contents (path, &text, length, &error))
    g_error ("%s: %s", path, error->message);
  return text;
}

BroJsonValue *
bro_test_fixture_json (const char *relative)
{
  gsize length = 0;
  g_autofree char *text = bro_test_fixture_text (relative, &length);
  BroJsonValue *v = bro_json_value_parse (text, (gssize) length);
  if (!v)
    g_error ("%s isn't JSON", relative);
  return v;
}

void
bro_test_fail (GPtrArray *failures, const char *id, const char *format, ...)
{
  va_list ap;
  va_start (ap, format);
  g_autofree char *msg = g_strdup_vprintf (format, ap);
  va_end (ap);
  g_ptr_array_add (failures, g_strdup_printf ("%s: %s", id, msg));
}

gboolean
bro_test_same_string (GPtrArray *failures, const char *id, const char *what, const char *expected, const char *actual)
{
  if (g_strcmp0 (expected, actual) == 0)
    return TRUE;
  bro_test_fail (failures, id, "%s: expected %s%s%s, got %s%s%s", what, expected ? "\"" : "", expected ? expected : "null",
                 expected ? "\"" : "", actual ? "\"" : "", actual ? actual : "null", actual ? "\"" : "");
  return FALSE;
}

gboolean
bro_test_same_json (GPtrArray *failures, const char *id, const char *what, const BroJsonValue *expected, const BroJsonValue *actual)
{
  g_autoptr (BroJsonValue) null_value = bro_json_value_new_null ();
  if (!expected)
    expected = null_value;
  if (!actual)
    actual = null_value;
  if (bro_json_value_equal (expected, actual))
    return TRUE;
  g_autofree char *e = bro_json_value_to_text (expected);
  g_autofree char *a = bro_json_value_to_text (actual);
  bro_test_fail (failures, id, "%s: expected %s     got %s", what, e, a);
  return FALSE;
}

void
bro_test_run_cases (const char *relative, BroTestCaseFunc run)
{
  bro_test_run_cases_only (relative, NULL, run);
}

void
bro_test_run_cases_only (const char *relative, gboolean (*only) (const char *id), BroTestCaseFunc run)
{
  g_autoptr (BroJsonValue) doc = bro_test_fixture_json (relative);
  BroJsonValue *cases = bro_json_value_member (doc, "cases");
  g_assert_cmpuint (bro_json_value_length (cases), >, 0);
  g_autoptr (GPtrArray) failures = g_ptr_array_new_with_free_func (g_free);
  guint run_count = 0;
  for (guint i = 0; i < bro_json_value_length (cases); i++) {
    BroJsonValue *c = bro_json_value_at (cases, i);
    const char *id = bro_json_value_get_string (bro_json_value_member (c, "id"), "?");
    if (only && !only (id))
      continue;
    run_count++;
    if (!run (id, bro_json_value_member (c, "given"), bro_json_value_member (c, "expect"), failures))
      bro_test_fail (failures, id, "no test handles this case");
  }
  if (run_count == 0) {
    g_printerr ("%s: no cases\n", relative);
    g_test_fail ();
  }
  if (failures->len) {
    GString *all = g_string_new (NULL);
    for (guint i = 0; i < failures->len; i++)
      g_string_append_printf (all, "\n  %s", (char *) failures->pdata[i]);
    g_test_message ("%s: %u of %u case(s) failed:%s", relative, failures->len, bro_json_value_length (cases), all->str);
    g_printerr ("%s: %u of %u case(s) failed:%s\n", relative, failures->len, bro_json_value_length (cases), all->str);
    g_string_free (all, TRUE);
    g_test_fail ();
  }
}

char *
bro_test_generated_listing (const BroJsonValue *disc)
{
  const char *volume = bro_json_value_get_string (bro_json_value_member (disc, "volume"), "");
  const char *type = bro_json_value_get_string (bro_json_value_member (disc, "type"), "bluray");
  BroJsonValue *titles = bro_json_value_member (disc, "titles");
  GString *s = g_string_new ("MSG:1005,0,1,\"MakeMKV v1.18.1 darwin(arm64-release) started\",\"%1 started\",\"MakeMKV v1.18.1 darwin(arm64-release)\"\n");
  g_string_append_printf (s, "TCOUNT:%u\n", bro_json_value_length (titles));
  if (strcmp (type, "dvd") == 0)
    g_string_append (s, "CINFO:1,6206,\"DVD disc\"\n");
  else if (strcmp (type, "hddvd") == 0)
    g_string_append (s, "CINFO:1,6207,\"HD-DVD disc\"\n");
  else
    g_string_append (s, "CINFO:1,6209,\"Blu-ray disc\"\n");
  g_string_append_printf (s, "CINFO:2,0,\"%s\"\nCINFO:32,0,\"%s\"\n", volume, volume);
  for (guint i = 0; i < bro_json_value_length (titles); i++) {
    BroJsonValue *t = bro_json_value_at (titles, i);
    gboolean pair = t->kind == BRO_JSON_VALUE_ARRAY;
    const char *duration = pair ? bro_json_value_get_string (bro_json_value_at (t, 0), "") : bro_json_value_get_string (bro_json_value_member (t, "duration"), "");
    gint64 source = pair ? bro_json_value_get_integer (bro_json_value_at (t, 1), 0) : bro_json_value_get_integer (bro_json_value_member (t, "source"), 0);
    BroJsonValue *size = pair ? NULL : bro_json_value_member (t, "size");
    gint64 chapters = pair ? 2 : bro_json_value_get_integer (bro_json_value_member (t, "chapters"), 2);
    g_string_append_printf (s, "TINFO:%u,8,0,\"%" G_GINT64_FORMAT "\"\nTINFO:%u,9,0,\"%s\"\nTINFO:%u,16,0,\"0000%" G_GINT64_FORMAT ".mpls\"\n",
                            i, chapters, i, duration, i, source);
    g_string_append_printf (s, "TINFO:%u,24,0,\"%" G_GINT64_FORMAT "\"\nTINFO:%u,26,0,\"%" G_GINT64_FORMAT "\"\nTINFO:%u,27,0,\"title_t0%u.mkv\"\n",
                            i, source, i, source, i, i);
    if (size)
      g_string_append_printf (s, "TINFO:%u,11,0,\"%" G_GINT64_FORMAT "\"\n", i, bro_json_value_get_integer (size, 0));
    g_string_append_printf (s, "SINFO:%u,0,1,6201,\"Video\"\n", i);
    if (strcmp (type, "uhd") == 0)
      g_string_append_printf (s, "SINFO:%u,0,19,0,\"3840x2160\"\n", i);
    g_string_append_printf (s, "SINFO:%u,1,1,6202,\"Audio\"\n", i);
  }
  g_string_append (s, "MSG:5011,0,0,\"Operation successfully completed\",\"Operation successfully completed\"\n");
  return g_string_free (s, FALSE);
}
