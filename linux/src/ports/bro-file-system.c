/* bro-file-system.c */
#include "bro-file-system.h"

G_DEFINE_INTERFACE (BroFileSystem, bro_file_system, G_TYPE_OBJECT)

static void
bro_file_system_default_init (BroFileSystemInterface *iface)
{
}

gboolean
bro_file_system_exists (BroFileSystem *self, const char *path)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->exists (self, path);
}

BroFileInfo *
bro_file_system_stat (BroFileSystem *self, const char *path, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->stat (self, path, error);
}

GPtrArray *
bro_file_system_list (BroFileSystem *self, const char *directory, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->list (self, directory, error);
}

GBytes *
bro_file_system_read (BroFileSystem *self, const char *path, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->read (self, path, error);
}

GBytes *
bro_file_system_read_range (BroFileSystem *self, const char *path, gint64 offset, int length, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->read_range (self, path, offset, length, error);
}

gboolean
bro_file_system_create_directory (BroFileSystem *self, const char *path, gboolean parents_must_exist, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->create_directory (self, path, parents_must_exist, error);
}

gboolean
bro_file_system_write_atomically (BroFileSystem *self, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->write_atomically (self, path, bytes, mode, error);
}

gboolean
bro_file_system_rename (BroFileSystem *self, const char *from, const char *to, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->rename (self, from, to, error);
}

GPtrArray *
bro_file_system_move_merging (BroFileSystem *self, const char *from, const char *to, BroMovePolicy policy, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->move_merging (self, from, to, policy, error);
}

gboolean
bro_file_system_remove (BroFileSystem *self, const char *path, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->remove (self, path, error);
}

gboolean
bro_file_system_move_to_trash (BroFileSystem *self, const char *path, const char *trash, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->move_to_trash (self, path, trash, error);
}

gboolean
bro_file_system_sync_file (BroFileSystem *self, const char *path, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->sync_file (self, path, error);
}

gboolean
bro_file_system_sync_directory (BroFileSystem *self, const char *path, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), FALSE);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->sync_directory (self, path, error);
}

BroByteStream *
bro_file_system_open_for_reading (BroFileSystem *self, const char *path, gboolean bypass_cache, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->open_for_reading (self, path, bypass_cache, error);
}

BroVolumeInfo *
bro_file_system_volume (BroFileSystem *self, const char *path, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_FILE_SYSTEM (self), NULL);
  return BRO_FILE_SYSTEM_GET_IFACE (self)->volume (self, path, error);
}
