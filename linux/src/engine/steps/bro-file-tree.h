/* bro-file-tree.h: BroFileTree, what is under a folder a job made (its staging, a backup in it), through the FileSystem
 * port. */
#pragma once

#include "bro-file-system.h"

G_BEGIN_DECLS

/* Every file under @root (char *, g_free) as a path relative to it ('/' between names), in byte order; an empty folder
 * is its path with a trailing '/'. NULL and @error set when a folder can't be listed. */
GPtrArray *bro_file_tree_files (BroFileSystem *fs, const char *root, BroBroError **error);

/* Removes a file, or a folder with everything in it (files first, then the folders, deepest first). */
gboolean bro_file_tree_remove (BroFileSystem *fs, const char *path, BroBroError **error);

/* Syncs every file under @path (or the file itself) to disk. */
gboolean bro_file_tree_sync (BroFileSystem *fs, const char *path, BroBroError **error);

G_END_DECLS
