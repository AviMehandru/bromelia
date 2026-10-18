/* bro-job-log.c */
#include "bro-job-log.h"

#include <string.h>

/* The canonical encoding on one line: its line breaks and their indentation are formatting only (a string never holds
 * a raw line break). */
static void
append_line (GString *out, BroJsonValue *value)
{
  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (value);
  gsize n;
  const char *s = g_bytes_get_data (bytes, &n);
  for (gsize i = 0; i < n; i++)
    {
      if (s[i] != '\n')
        {
          g_string_append_c (out, s[i]);
          continue;
        }
      while (i + 1 < n && s[i + 1] == ' ')
        i++;
    }
  g_string_append_c (out, '\n');
}

GBytes *
bro_job_log_encode (GPtrArray *lines)
{
  GString *text = g_string_new ("");
  for (guint i = 0; i < lines->len; i++)
    {
      g_autoptr (BroJsonValue) line = bro_log_line_dto_json (g_ptr_array_index (lines, i));
      g_autoptr (BroJsonValue) without = bro_json_value_new_object ();
      for (guint k = 0; k < line->keys->len; k++)
        if (strcmp (g_ptr_array_index (line->keys, k), "seq") != 0)
          bro_json_value_set (without, g_ptr_array_index (line->keys, k), bro_json_value_ref (g_ptr_array_index (line->items, k)));
      append_line (text, without);
    }
  return g_string_free_to_bytes (text);
}

gboolean
bro_job_log_append (BroFileSystem *fs, const char *job_directory, GPtrArray *lines, BroBroError **error)
{
  g_autofree char *path = g_strconcat (job_directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
  g_autoptr (GBytes) before = NULL;
  g_autoptr (GBytes) added = NULL;
  g_autoptr (GByteArray) all = NULL;
  gboolean ok;
  if (lines->len == 0)
    return TRUE;
  if (!bro_file_system_exists (fs, job_directory) && !bro_file_system_create_directory (fs, job_directory, FALSE, error))
    return FALSE;
  if (bro_file_system_exists (fs, path) && !(before = bro_file_system_read (fs, path, error)))
    return FALSE;
  added = bro_job_log_encode (lines);
  all = g_byte_array_new ();
  if (before)
    g_byte_array_append (all, g_bytes_get_data (before, NULL), g_bytes_get_size (before));
  g_byte_array_append (all, g_bytes_get_data (added, NULL), g_bytes_get_size (added));
  {
    g_autoptr (GBytes) bytes = g_bytes_new (all->data, all->len);
    ok = bro_file_system_write_atomically (fs, path, bytes, 0600, error);
  }
  return ok;
}

GPtrArray *
bro_job_log_decode (GBytes *bytes)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_line_dto_free);
  gsize n = 0;
  const char *data = bytes ? g_bytes_get_data (bytes, &n) : NULL;
  gsize start = 0;
  gint64 seq = 0;
  for (gsize i = 0; i < n; i++)
    {
      g_autoptr (BroJsonValue) j = NULL;
      BroInstant t;
      BroSeverity severity;
      BroMessageCode code;
      BroJsonValue *params;
      if (data[i] != '\n')
        continue;
      seq++;
      j = bro_json_value_parse (data + start, (gssize) (i - start));
      start = i + 1;
      if (!j || !bro_instant_parse (bro_json_value_get_string (bro_json_value_member (j, "t"), ""), &t)
          || !bro_severity_from_wire (bro_json_value_get_string (bro_json_value_member (j, "severity"), ""), &severity)
          || !bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (j, "code"), ""), &code))
        continue;
      params = bro_json_value_member (j, "params");
      g_ptr_array_add (lines, bro_log_line_dto_new (seq, t, severity, code, params && params->kind == BRO_JSON_VALUE_OBJECT ? params : NULL));
    }
  return lines;
}
