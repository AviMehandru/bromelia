/* bro-layouts.h: BroLayouts, where a job's files go: the profile's templates, or the names Plex, Jellyfin and Emby
 * expect. @values: char * → char *. */
#pragma once

#include "bro-naming-settings.h"
#include "bro-planned-output.h"
#include "bro-planned-path.h"

G_BEGIN_DECLS

/* The unit's folder, relative to the library: the folder template, or Movies/Name (Year) / TV Shows/Name (Year) (by
 * kind unless libraryFolder is set). Free with g_free. */
char *bro_layouts_folder (const BroNamingSettings *naming, GHashTable *values);

/* The path (BroPlannedPath *) of each output (BroPlannedOutput *), relative to the library: the unit's folder,
 * then the file. Media server: episodes in Season NN, a movie's main feature by its name, other titles in Other,
 * backups and images in Backup. Templates: the file name template (MakeMKV's name, original, when it's empty); a
 * backup goes in the backup subfolder when there is one. */
GPtrArray *bro_layouts_paths (const BroNamingSettings *naming, GHashTable *values, GPtrArray *outputs);

G_END_DECLS
