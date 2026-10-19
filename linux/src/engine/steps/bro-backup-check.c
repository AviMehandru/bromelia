/* bro-backup-check.c */
#include "bro-backup-check.h"
#include "bro-backup-structure.h"
#include "bro-file-info.h"
#include "bro-file-tree.h"
#include "bro-steps-private.h"

#include <string.h>

BroBroMessage *
bro_backup_check_problem (BroFileSystem *fs, const char *path)
{
  const char *slash = strrchr (path, '/');
  const char *name = slash ? slash + 1 : path;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroFileInfo) info = NULL;
  if (!bro_file_system_exists (fs, path))
    return bro_backup_structure_problem (name, FALSE, NULL, NULL);
  if ((info = bro_file_system_stat (fs, path, &error)))
    {
      if (info->is_directory)
        {
          g_autoptr (GPtrArray) files = bro_file_tree_files (fs, path, &error);
          if (files)
            {
              g_ptr_array_add (files, NULL);
              return bro_backup_structure_problem (name, FALSE, (const char *const *) files->pdata, NULL);
            }
        }
      else
        {
          g_autoptr (GBytes) header = bro_file_system_read_range (fs, path, 32769, 5, &error);
          if (header)
            return bro_backup_structure_problem (name, TRUE, NULL, header);
        }
    }
  return bro_bro_message_new (BRO_MSG_STRUCTURE_UNREADABLE, _bro_param ("name", bro_json_value_new_string (name)), BRO_SEVERITY_INFO);
}
