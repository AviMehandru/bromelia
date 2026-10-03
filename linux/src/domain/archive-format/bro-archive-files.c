/* bro-archive-files.c */
#include "bro-archive-files.h"

#include <string.h>

gboolean
bro_archive_files_is_own_file (const char *name)
{
  g_autoptr (GRegex) own_log = g_regex_new ("^(bromelia|makemkv)(-[0-9a-f]{8})?-(log|debug-log)([- (].*)?\\.txt$", 0, 0, NULL);
  return strcmp (name, "SHA256SUMS") == 0 || (g_str_has_prefix (name, "bromelia") && g_str_has_suffix (name, ".json"))
         || g_regex_match (own_log, name, 0, NULL) || strcmp (name, "INCOMPLETE.txt") == 0 || strcmp (name, "READ ERRORS.txt") == 0;
}

gboolean
bro_archive_files_is_metadata_file (const char *name)
{
  gsize n = strlen (name);
  return (n >= 4 && g_ascii_strcasecmp (name + n - 4, ".nfo") == 0) || strcmp (name, "poster.jpg") == 0;
}

static gboolean
hidden (const char *path)
{
  if (path[0] == '.')
    return TRUE;
  return strstr (path, "/.") != NULL;
}

static int
by_bytes (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}

GStrv
bro_archive_files_expand (const char *const *produced, const char *const *tree)
{
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *out = g_ptr_array_new ();
  for (int i = 0; produced && produced[i]; i++) {
    g_autofree char *item = g_strdup (produced[i]);
    gsize n = strlen (item);
    while (n > 0 && item[n - 1] == '/')
      item[--n] = '\0';
    for (int k = 0; tree && tree[k]; k++) {
      const char *f = tree[k];
      gboolean under = strcmp (f, item) == 0 || (g_str_has_prefix (f, item) && f[n] == '/');
      if (under && !hidden (f) && !g_hash_table_contains (seen, f)) {
        g_hash_table_add (seen, (gpointer) f);
        g_ptr_array_add (out, g_strdup (f));
      }
    }
  }
  g_ptr_array_sort (out, by_bytes);
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}
