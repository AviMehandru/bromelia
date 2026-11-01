/* bro-scenario-catalog.c */
#include "bro-scenario-catalog.h"

#include <string.h>

char *
bro_scenario_catalog_folder (void)
{
  const char *fixtures = g_getenv ("BROMELIA_FIXTURES");
  g_autofree char *folder = g_build_filename (fixtures ? fixtures : ".", "..", "scenarios", "json", NULL);
  return g_canonicalize_filename (folder, NULL);
}

static int
by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

GPtrArray *
bro_scenario_catalog_selected (void)
{
  g_autofree char *folder = bro_scenario_catalog_folder ();
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) dir = g_dir_open (folder, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *id = NULL;
      g_autoptr (BroJsonValue) scenario = NULL;
      if (!g_str_has_suffix (name, ".json"))
        continue;
      id = g_strndup (name, strlen (name) - 5);
      scenario = bro_scenario_catalog_load (id);
      if (scenario && bro_scenario_catalog_selects (scenario))
        g_ptr_array_add (ids, g_steal_pointer (&id));
    }
  g_ptr_array_sort (ids, by_text);
  return ids;
}

gboolean
bro_scenario_catalog_selects (const BroJsonValue *scenario)
{
  BroJsonValue *phase = bro_json_value_member (scenario, "phase"), *platforms = bro_json_value_member (scenario, "platforms");
  gboolean named = platforms == NULL || platforms->kind != BRO_JSON_VALUE_ARRAY;
  for (guint i = 0; !named && i < bro_json_value_length (platforms); i++)
    named = g_strcmp0 (bro_json_value_get_string (bro_json_value_at (platforms, i), NULL), BRO_SCENARIO_PLATFORM) == 0;
  return bro_json_value_member (scenario, "steps") && !bro_json_value_member (scenario, "realDisc") && phase
         && phase->kind == BRO_JSON_VALUE_INTEGER && phase->integer <= BRO_SCENARIO_BUILT_PHASE && named;
}

BroJsonValue *
bro_scenario_catalog_load (const char *id)
{
  g_autofree char *file = g_strconcat (id, ".json", NULL);
  return bro_scenario_catalog_read (file);
}

BroJsonValue *
bro_scenario_catalog_read (const char *relative)
{
  g_autofree char *folder = bro_scenario_catalog_folder ();
  g_autofree char *stem = g_str_has_suffix (relative, ".yaml") ? g_strndup (relative, strlen (relative) - 5) : NULL;
  g_autofree char *name = stem ? g_strconcat (stem, ".json", NULL) : g_strdup (relative);
  g_autofree char *path = g_build_filename (folder, name, NULL);
  g_autofree char *text = NULL;
  gsize length;
  if (!g_file_get_contents (path, &text, &length, NULL))
    return NULL;
  return bro_json_value_parse (text, (gssize) length);
}

BroJsonValue *
bro_scenario_catalog_disc (BroJsonValue *disc)
{
  if (disc && disc->kind == BRO_JSON_VALUE_STRING)
    {
      g_autofree char *file = g_strconcat ("discs/", disc->string, ".json", NULL);
      return bro_scenario_catalog_read (file);
    }
  return disc ? bro_json_value_ref (disc) : NULL;
}

BroJsonValue *
bro_scenario_catalog_script (BroJsonValue *script)
{
  if (script && script->kind == BRO_JSON_VALUE_STRING)
    return bro_scenario_catalog_read (script->string);
  return script ? bro_json_value_ref (script) : NULL;
}

GHashTable *
bro_scenario_catalog_lines (void)
{
  g_autoptr (BroJsonValue) lines = bro_scenario_catalog_read ("tools/lines.json");
  GHashTable *named = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  for (guint i = 0; lines && lines->kind == BRO_JSON_VALUE_OBJECT && i < lines->keys->len; i++)
    g_hash_table_insert (named, g_strdup (lines->keys->pdata[i]), g_strdup (bro_json_value_get_string (lines->items->pdata[i], "")));
  return named;
}
