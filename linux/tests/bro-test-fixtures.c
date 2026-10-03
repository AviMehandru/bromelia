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
  g_autoptr (BroJsonValue) doc = bro_test_fixture_json (relative);
  BroJsonValue *cases = bro_json_value_member (doc, "cases");
  g_assert_cmpuint (bro_json_value_length (cases), >, 0);
  g_autoptr (GPtrArray) failures = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < bro_json_value_length (cases); i++) {
    BroJsonValue *c = bro_json_value_at (cases, i);
    const char *id = bro_json_value_get_string (bro_json_value_member (c, "id"), "?");
    if (!run (id, bro_json_value_member (c, "given"), bro_json_value_member (c, "expect"), failures))
      bro_test_fail (failures, id, "no test handles this case");
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
