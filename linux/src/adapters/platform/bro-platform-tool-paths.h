/* bro-platform-tool-paths.h: BroPlatformToolPaths: where each tool is usually installed on Linux, and its program
 * names (today's find_tool folders: /opt/homebrew/bin, /usr/local/bin, /usr/bin). */
#pragma once

#include "bro-tool-kind.h"
#include <glib.h>

G_BEGIN_DECLS

/* BroToolKind (GINT_TO_POINTER) → GStrv. Free with g_hash_table_unref. */
GHashTable *bro_platform_tool_paths_candidates (const char *home);

/* BroToolKind → GStrv of program names. Free with g_hash_table_unref. */
GHashTable *bro_platform_tool_paths_names (void);

G_END_DECLS
