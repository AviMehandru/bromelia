/* bro-settings-conf.c */
#include "bro-settings-conf.h"

#include <string.h>

GHashTable *
bro_settings_conf_parse (const char *text)
{
  GHashTable *o = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    char *t = g_strstrip (lines[i]);
    char *eq = strchr (t, '=');
    if (!*t || *t == '#' || !eq)
      continue;
    *eq = '\0';
    char *key = g_strstrip (t), *value = g_strstrip (eq + 1);
    gsize n = strlen (value);
    if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
      value[n - 1] = '\0';
      value++;
    }
    if (*key)
      g_hash_table_replace (o, g_strdup (key), g_strdup (value));
  }
  return o;
}

static int
by_string (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

char *
bro_settings_conf_render (GHashTable *settings, const char *header)
{
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "#\n# MakeMKV settings file. %s.\n# Changes made here are overwritten before every job.\n#\n\n", header);
  g_autoptr (GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (settings);
  g_ptr_array_sort (keys, by_string);
  for (guint i = 0; i < keys->len; i++) {
    g_autofree char *value = g_strdup (g_hash_table_lookup (settings, keys->pdata[i]));
    for (char *p = value; *p; p++)
      if (*p == '"')
        *p = '\'';
      else if (*p == '\n')
        *p = ' ';
    g_string_append_printf (s, "%s = \"%s\"\n", (char *) keys->pdata[i], value);
  }
  return g_string_free (s, FALSE);
}
