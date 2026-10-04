/* bro-system-tool-locator.c */
#include "bro-system-tool-locator.h"

#include "bro-message-code.h"
#include <string.h>

struct _BroSystemToolLocator {
  GObject parent_instance;
  BroFileSystem *fs;
  GHashTable *configured, *candidates, *names;
  GStrv search_path;
  char *home;
};

static void bro_system_tool_locator_iface_init (BroToolLocatorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSystemToolLocator, bro_system_tool_locator, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_TOOL_LOCATOR, bro_system_tool_locator_iface_init))

static char *
expand (BroSystemToolLocator *self, const char *path)
{
  return g_str_has_prefix (path, "~/") ? g_build_filename (self->home, path + 2, NULL) : g_strdup (path);
}

static gboolean
is_file (BroSystemToolLocator *self, const char *path)
{
  g_autoptr (BroFileInfo) info = bro_file_system_stat (self->fs, path, NULL);
  return info && !info->is_directory;
}

static BroToolInfo *
found (BroToolKind tool, const char *path)
{
  BroToolInfo *info = bro_tool_info_new ();
  info->tool = tool;
  info->path = g_strdup (path);
  return info;
}

static BroToolInfo *
not_found (BroToolKind tool, BroMessageCode code, const char *path)
{
  BroToolInfo *info = bro_tool_info_new ();
  BroJsonValue *params = bro_json_value_new_object ();
  info->tool = tool;
  bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (tool)));
  if (path)
    bro_json_value_set (params, "path", bro_json_value_new_string (path));
  info->why = bro_bro_message_new (code, params, path ? BRO_SEVERITY_ERROR : BRO_SEVERITY_WARNING);
  return info;
}

/* The first @name in @dir that is a file, or NULL. */
static char *
first_in (BroSystemToolLocator *self, const char *dir, BroToolKind tool)
{
  const char *const *names = g_hash_table_lookup (self->names, GINT_TO_POINTER (tool));
  const char *fallback[] = { bro_tool_kind_to_wire (tool), NULL };
  if (!names)
    names = fallback;
  for (guint i = 0; names[i]; i++)
    {
      char *path = g_build_filename (dir, names[i], NULL);
      if (is_file (self, path))
        return path;
      g_free (path);
    }
  return NULL;
}

BroToolInfo *
bro_system_tool_locator_locate (BroSystemToolLocator *self, BroToolKind tool)
{
  const char *set = g_hash_table_lookup (self->configured, GINT_TO_POINTER (tool));
  const char *const *candidates = g_hash_table_lookup (self->candidates, GINT_TO_POINTER (tool));
  g_autofree char *trimmed = g_strstrip (g_strdup (set ? set : ""));
  if (*trimmed)
    {
      g_autofree char *path = expand (self, trimmed);
      return is_file (self, path) ? found (tool, path) : not_found (tool, BRO_MSG_TOOL_NOT_FOUND_AT, path);
    }
  if (tool == BRO_TOOL_KIND_MKVEXTRACT)
    {
      g_autoptr (BroToolInfo) mkvmerge = bro_system_tool_locator_locate (self, BRO_TOOL_KIND_MKVMERGE);
      if (mkvmerge->path)
        {
          g_autofree char *dir = g_path_get_dirname (mkvmerge->path);
          g_autofree char *path = first_in (self, dir, tool);
          if (path)
            return found (tool, path);
        }
    }
  for (guint i = 0; candidates && candidates[i]; i++)
    {
      g_autofree char *path = expand (self, candidates[i]);
      if (is_file (self, path))
        return found (tool, path);
    }
  for (guint i = 0; self->search_path && self->search_path[i]; i++)
    {
      g_autofree char *path = *self->search_path[i] ? first_in (self, self->search_path[i], tool) : NULL;
      if (path)
        return found (tool, path);
    }
  return not_found (tool, BRO_MSG_TOOL_MISSING, NULL);
}

static BroToolInfo *
locate_vfunc (BroToolLocator *self, BroToolKind tool)
{
  return bro_system_tool_locator_locate (BRO_SYSTEM_TOOL_LOCATOR (self), tool);
}

static void
bro_system_tool_locator_finalize (GObject *object)
{
  BroSystemToolLocator *self = BRO_SYSTEM_TOOL_LOCATOR (object);
  g_clear_object (&self->fs);
  g_clear_pointer (&self->configured, g_hash_table_unref);
  g_clear_pointer (&self->candidates, g_hash_table_unref);
  g_clear_pointer (&self->names, g_hash_table_unref);
  g_strfreev (self->search_path);
  g_free (self->home);
  G_OBJECT_CLASS (bro_system_tool_locator_parent_class)->finalize (object);
}

static void
bro_system_tool_locator_class_init (BroSystemToolLocatorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_system_tool_locator_finalize;
}

static void
bro_system_tool_locator_init (BroSystemToolLocator *self)
{
}

static void
bro_system_tool_locator_iface_init (BroToolLocatorInterface *iface)
{
  iface->locate = locate_vfunc;
}

BroSystemToolLocator *
bro_system_tool_locator_new (BroFileSystem *fs, GHashTable *configured, GHashTable *candidates, GHashTable *names,
                             const char *const *search_path, const char *home)
{
  BroSystemToolLocator *self = g_object_new (BRO_TYPE_SYSTEM_TOOL_LOCATOR, NULL);
  self->fs = g_object_ref (fs);
  self->configured = g_hash_table_ref (configured);
  self->candidates = g_hash_table_ref (candidates);
  self->names = g_hash_table_ref (names);
  self->search_path = g_strdupv ((char **) search_path);
  self->home = g_strdup (home);
  return self;
}
