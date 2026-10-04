/* bro-makemkv-run-settings.h: one makemkvcon run's settings: settings.conf (SettingsLayers.merge), the generated
 * profile (ProfileXml.render; none: no profile file) and the user's MakeMKV data folder. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  GHashTable *settings; /* char * → char * */
  char *profile_xml; /* nullable */
  char *data_dir;
} BroMakemkvRunSettings;

/* Empty lists, nothing else set. */
BroMakemkvRunSettings *bro_makemkv_run_settings_new (void);
void bro_makemkv_run_settings_free (BroMakemkvRunSettings *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvRunSettings, bro_makemkv_run_settings_free)

G_END_DECLS
