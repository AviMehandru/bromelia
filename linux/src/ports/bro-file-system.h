/* bro-file-system.h: BroFileSystem: Files and folders. Paths are absolute. */
#pragma once

#include "bro-bro-error.h"
#include "bro-byte-stream.h"
#include "bro-directory-entry.h"
#include "bro-file-info.h"
#include "bro-move-policy.h"
#include "bro-moved-item.h"
#include "bro-volume-info.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_FILE_SYSTEM (bro_file_system_get_type ())
G_DECLARE_INTERFACE (BroFileSystem, bro_file_system, BRO, FILE_SYSTEM, GObject)

struct _BroFileSystemInterface {
  GTypeInterface parent_iface;

  gboolean (*exists) (BroFileSystem *self, const char *path);
  BroFileInfo *(*stat) (BroFileSystem *self, const char *path, BroBroError **error);
  GPtrArray *(*list) (BroFileSystem *self, const char *directory, BroBroError **error);
  GBytes *(*read) (BroFileSystem *self, const char *path, BroBroError **error);
  GBytes *(*read_range) (BroFileSystem *self, const char *path, gint64 offset, int length, BroBroError **error);
  gboolean (*create_directory) (BroFileSystem *self, const char *path, gboolean parents_must_exist, BroBroError **error);
  gboolean (*write_atomically) (BroFileSystem *self, const char *path, GBytes *bytes, int mode, BroBroError **error);
  gboolean (*rename) (BroFileSystem *self, const char *from, const char *to, BroBroError **error);
  GPtrArray *(*move_merging) (BroFileSystem *self, const char *from, const char *to, BroMovePolicy policy, BroMovedFunc on_moved,
                              gpointer data, BroBroError **error);
  gboolean (*remove) (BroFileSystem *self, const char *path, BroBroError **error);
  gboolean (*move_to_trash) (BroFileSystem *self, const char *path, const char *trash, BroBroError **error);
  gboolean (*sync_file) (BroFileSystem *self, const char *path, BroBroError **error);
  gboolean (*sync_directory) (BroFileSystem *self, const char *path, BroBroError **error);
  BroByteStream *(*open_for_reading) (BroFileSystem *self, const char *path, gboolean bypass_cache, BroBroError **error);
  BroVolumeInfo *(*volume) (BroFileSystem *self, const char *path, BroBroError **error);
};

/* Whether anything is at path. */
gboolean bro_file_system_exists (BroFileSystem *self, const char *path);

BroFileInfo *bro_file_system_stat (BroFileSystem *self, const char *path, BroBroError **error);

/* The entries of a folder, in no particular order. */
GPtrArray *bro_file_system_list (BroFileSystem *self, const char *directory, BroBroError **error);

GBytes *bro_file_system_read (BroFileSystem *self, const char *path, BroBroError **error);

/* Fewer bytes at the end of the file. */
GBytes *bro_file_system_read_range (BroFileSystem *self, const char *path, gint64 offset, int length, BroBroError **error);

/* Creates a folder (and, unless parentsMustExist, its parents). Never creates a library root (plan §22). FALSE and
 * @error set on failure. */
gboolean bro_file_system_create_directory (BroFileSystem *self, const char *path, gboolean parents_must_exist, BroBroError **error);

/* A temporary file, fsync, rename over path, fsync of the folder; mode is the POSIX permission bits. FALSE and @error
 * set on failure. A folder sync that fails fails the call although the new content is in place: a retry replaces it, so
 * it is safe. */
gboolean bro_file_system_write_atomically (BroFileSystem *self, const char *path, GBytes *bytes, int mode, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_file_system_rename (BroFileSystem *self, const char *from, const char *to, BroBroError **error);

/* Moves a folder's contents into another, merging folders. @on_moved (nullable) hears of each item as soon as it has
 * moved, so a failure part-way (NULL and @error set) still says what moved. */
GPtrArray *bro_file_system_move_merging (BroFileSystem *self, const char *from, const char *to, BroMovePolicy policy, BroMovedFunc on_moved,
                                         gpointer data, BroBroError **error);

/* Removes a file or an empty folder. FALSE and @error set on failure. */
gboolean bro_file_system_remove (BroFileSystem *self, const char *path, BroBroError **error);

/* Moves path into the trash folder (never deletes). FALSE and @error set on failure. */
gboolean bro_file_system_move_to_trash (BroFileSystem *self, const char *path, const char *trash, BroBroError **error);

/* F_FULLFSYNC / FlushFileBuffers / fsync. FALSE and @error set on failure. */
gboolean bro_file_system_sync_file (BroFileSystem *self, const char *path, BroBroError **error);

/* Syncs a folder after a rename in it; succeeds when the file system can't sync folders at all. FALSE and @error
 * set on failure. */
gboolean bro_file_system_sync_directory (BroFileSystem *self, const char *path, BroBroError **error);

/* F_NOCACHE / NO_BUFFERING / fadvise when bypassCache. */
BroByteStream *bro_file_system_open_for_reading (BroFileSystem *self, const char *path, gboolean bypass_cache, BroBroError **error);

BroVolumeInfo *bro_file_system_volume (BroFileSystem *self, const char *path, BroBroError **error);

G_END_DECLS
