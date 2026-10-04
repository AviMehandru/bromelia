/* bro-settings-layers.h: BroSettingsLayers, the settings.conf a job's makemkvcon reads. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* @global (makemkv.settings), then the profile's, then the drive's (each char * → char *, nullable); empty values are
 * dropped. app_Key is the registration key when there is one; app_DefaultSelectionString the selection override
 * (hand-picked tracks: +sel:all) when there is one; app_DataDir, unless a layer sets it, the user's real MakeMKV data
 * folder (the job runs with its own home). A new char * → char * table. */
GHashTable *bro_settings_layers_merge (GHashTable *global, GHashTable *profile, GHashTable *drive, const char *registration_key,
                                       const char *selection_override, const char *data_dir);

G_END_DECLS
