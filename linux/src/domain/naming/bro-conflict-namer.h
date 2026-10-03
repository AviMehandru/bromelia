/* bro-conflict-namer.h: BroConflictNamer, names that don't collide. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* @name when no entry of @existing has it (ignoring ASCII case, as some file systems do), else "Name (2)",
 * "Name (3)" … before the extension of a file. Free with g_free. */
char *bro_conflict_namer_next (const char *name, const char *const *existing, gboolean is_folder);

G_END_DECLS
