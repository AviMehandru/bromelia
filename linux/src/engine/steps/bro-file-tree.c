/* bro-file-tree.c */
#include "bro-file-tree.h"
#include "bro-directory-entry.h"
#include "bro-file-info.h"

#include <string.h>

static int
by_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

static int
by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

static gboolean
walk (BroFileSystem *fs, const char *folder, const char *prefix, GPtrArray *files, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = bro_file_system_list (fs, folder, error);
  if (!entries)
    return FALSE;
  if (entries->len == 0 && *prefix)
    g_ptr_array_add (files, g_strdup (prefix));
  for (guint i = 0; i < entries->len; i++)
    {
      BroDirectoryEntry *e = entries->pdata[i];
      if (e->is_directory)
        {
          g_autofree char *path = g_strconcat (folder, "/", e->name, NULL);
          g_autofree char *inner = g_strconcat (prefix, e->name, "/", NULL);
          if (!walk (fs, path, inner, files, error))
            return FALSE;
        }
      else
        g_ptr_array_add (files, g_strconcat (prefix, e->name, NULL));
    }
  return TRUE;
}

GPtrArray *
bro_file_tree_files (BroFileSystem *fs, const char *root, BroBroError **error)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (g_free);
  if (!walk (fs, root, "", files, error))
    {
      g_ptr_array_unref (files);
      return NULL;
    }
  g_ptr_array_sort (files, by_text);
  return files;
}

gboolean
bro_file_tree_remove (BroFileSystem *fs, const char *path, BroBroError **error)
{
  g_autoptr (BroFileInfo) info = bro_file_system_stat (fs, path, error);
  if (!info)
    return FALSE;
  if (info->is_directory)
    {
      g_autoptr (GPtrArray) entries = bro_file_system_list (fs, path, error);
      if (!entries)
        return FALSE;
      g_ptr_array_sort (entries, by_name);
      for (guint i = 0; i < entries->len; i++)
        {
          g_autofree char *inner = g_strconcat (path, "/", ((BroDirectoryEntry *) entries->pdata[i])->name, NULL);
          if (!bro_file_tree_remove (fs, inner, error))
            return FALSE;
        }
    }
  return bro_file_system_remove (fs, path, error);
}

gboolean
bro_file_tree_sync (BroFileSystem *fs, const char *path, BroBroError **error)
{
  g_autoptr (BroFileInfo) info = bro_file_system_stat (fs, path, error);
  g_autoptr (GPtrArray) files = NULL;
  if (!info)
    return FALSE;
  if (!info->is_directory)
    return bro_file_system_sync_file (fs, path, error);
  if (!(files = bro_file_tree_files (fs, path, error)))
    return FALSE;
  for (guint i = 0; i < files->len; i++)
    {
      const char *file = files->pdata[i];
      g_autofree char *full = NULL;
      if (g_str_has_suffix (file, "/"))
        continue;
      full = g_strconcat (path, "/", file, NULL);
      if (!bro_file_system_sync_file (fs, full, error))
        return FALSE;
    }
  return TRUE;
}
