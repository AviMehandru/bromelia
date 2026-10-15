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
