/* bro-platform-file-system.h: BroPlatformFileSystem: the FileSystem port on Linux (plan §9, §10.3;
 * shared/fixtures/adapters/file-system.cases.json): writes are flushed with fsync (the file, then its folder), renames
 * never replace (renameat2 with RENAME_NOREPLACE) except write_atomically's, a missing parent is created only when
 * asked, uncached reads drop the file from the page cache first (posix_fadvise DONTNEED), and the volume comes from
 * statfs. Also builds on macOS, for the tests (F_FULLFSYNC, renamex_np, F_NOCACHE there). */
#pragma once

#include "bro-file-system.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLATFORM_FILE_SYSTEM (bro_platform_file_system_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformFileSystem, bro_platform_file_system, BRO, PLATFORM_FILE_SYSTEM, GObject)

BroPlatformFileSystem *bro_platform_file_system_new (void);

gboolean bro_platform_file_system_exists (BroPlatformFileSystem *self, const char *path);
BroFileInfo *bro_platform_file_system_stat (BroPlatformFileSystem *self, const char *path, BroBroError **error);
/* BroDirectoryEntry *, in no particular order. */
GPtrArray *bro_platform_file_system_list (BroPlatformFileSystem *self, const char *directory, BroBroError **error);
GBytes *bro_platform_file_system_read (BroPlatformFileSystem *self, const char *path, BroBroError **error);
GBytes *bro_platform_file_system_read_range (BroPlatformFileSystem *self, const char *path, gint64 offset, int length,
                                             BroBroError **error);
gboolean bro_platform_file_system_create_directory (BroPlatformFileSystem *self, const char *path, gboolean parents_must_exist,
                                                    BroBroError **error);
gboolean bro_platform_file_system_write_atomically (BroPlatformFileSystem *self, const char *path, GBytes *bytes, int mode,
                                                    BroBroError **error);
gboolean bro_platform_file_system_rename (BroPlatformFileSystem *self, const char *from, const char *to, BroBroError **error);
/* BroMovedItem *, sorted by source path; @on_moved (nullable) hears of each as soon as it has moved. */
GPtrArray *bro_platform_file_system_move_merging (BroPlatformFileSystem *self, const char *from, const char *to,
                                                  BroMovePolicy policy, BroMovedFunc on_moved, gpointer data, BroBroError **error);
gboolean bro_platform_file_system_remove (BroPlatformFileSystem *self, const char *path, BroBroError **error);
gboolean bro_platform_file_system_move_to_trash (BroPlatformFileSystem *self, const char *path, const char *trash,
                                                 BroBroError **error);
gboolean bro_platform_file_system_sync_file (BroPlatformFileSystem *self, const char *path, BroBroError **error);
gboolean bro_platform_file_system_sync_directory (BroPlatformFileSystem *self, const char *path, BroBroError **error);
BroByteStream *bro_platform_file_system_open_for_reading (BroPlatformFileSystem *self, const char *path, gboolean bypass_cache,
                                                          BroBroError **error);
/* The volume holding @path, or its nearest existing parent. */
BroVolumeInfo *bro_platform_file_system_volume (BroPlatformFileSystem *self, const char *path, BroBroError **error);

G_END_DECLS
