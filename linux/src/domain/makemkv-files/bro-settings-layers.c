/* bro-settings-layers.c */
#include "bro-settings-layers.h"

static void
overlay (GHashTable *o, GHashTable *layer)
{
  GHashTableIter it;
  gpointer k, v;
  if (!layer)
    return;
  g_hash_table_iter_init (&it, layer);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_replace (o, g_strdup (k), g_strdup (v));
}

static gboolean
is_empty (gpointer key, gpointer value, gpointer data)
{
  return !value || !*(const char *) value;
}

GHashTable *
bro_settings_layers_merge (GHashTable *global, GHashTable *profile, GHashTable *drive, const char *registration_key,
                           const char *selection_override, const char *data_dir)
{
  GHashTable *o = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  overlay (o, global);
  overlay (o, profile);
  overlay (o, drive);
  if (registration_key && *registration_key)
    g_hash_table_replace (o, g_strdup ("app_Key"), g_strdup (registration_key));
  if (selection_override)
    g_hash_table_replace (o, g_strdup ("app_DefaultSelectionString"), g_strdup (selection_override));
  const char *dd = g_hash_table_lookup (o, "app_DataDir");
  if (data_dir && *data_dir && !(dd && *dd))
    g_hash_table_replace (o, g_strdup ("app_DataDir"), g_strdup (data_dir));
  g_hash_table_foreach_remove (o, is_empty, NULL);
  return o;
}
