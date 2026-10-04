/* bro-platform-tool-paths.c */
#include "bro-platform-tool-paths.h"

static const char *
program (BroToolKind tool)
{
  return tool == BRO_TOOL_KIND_HANDBRAKE ? "HandBrakeCLI" : bro_tool_kind_to_wire (tool);
}

GHashTable *
bro_platform_tool_paths_candidates (const char *home)
{
  static const char *const dirs[] = { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", NULL };
  GHashTable *t = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_strfreev);
  for (int tool = BRO_TOOL_KIND_MAKEMKVCON; tool <= BRO_TOOL_KIND_APPRISE; tool++)
    {
      GPtrArray *list = g_ptr_array_new ();
      for (int i = 0; dirs[i]; i++)
        g_ptr_array_add (list, g_build_filename (dirs[i], program (tool), NULL));
      if (tool == BRO_TOOL_KIND_APPRISE)
        g_ptr_array_add (list, g_build_filename (home, ".local", "bin", "apprise", NULL)); /* pip install --user */
      g_ptr_array_add (list, NULL);
      g_hash_table_insert (t, GINT_TO_POINTER (tool), g_ptr_array_free (list, FALSE));
    }
  return t;
}

GHashTable *
bro_platform_tool_paths_names (void)
{
  GHashTable *t = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_strfreev);
  for (int tool = BRO_TOOL_KIND_MAKEMKVCON; tool <= BRO_TOOL_KIND_APPRISE; tool++)
    {
      char **names = g_new0 (char *, 2);
      names[0] = g_strdup (program (tool));
      g_hash_table_insert (t, GINT_TO_POINTER (tool), names);
    }
  return t;
}
