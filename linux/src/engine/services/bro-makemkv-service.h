/* bro-makemkv-service.h: BroMakemkvService: MakeMKV as the engine sets it up (plan §11.1), as far as phase 3 needs it:
 * what each makemkvcon run gets (BroMakemkvSetup). The settings are SettingsLayers.merge of the configuration's
 * makemkv.settings and the profile's (the drive's own settings are already in it), with the registration key
 * (makemkv.registrationKey through bro_config_service_secret), else the key MakeMKV is registered with (app_Key in the
 * user's settings.conf, on macOS and Linux, where a run reads only its own; the registry keeps it on Windows), and the
 * user's MakeMKV data folder. The profile file is the generated one (ProfileXml.render) or the custom file, the
 * switches come from the profile's makemkv, and the run lives in the job's home folder with its transcript in the
 * job's makemkv.txt. Version, registration state and the beta key are phase 5. Every call is safe from any thread; it
 * reads the user's settings.conf, so not on the executor. shared/fixtures/engine/makemkv-service.cases.json. */
#pragma once

#include "bro-config-service.h"
#include "bro-file-system.h"
#include "bro-makemkv-setup.h"

G_BEGIN_DECLS

#define BRO_TYPE_MAKEMKV_SERVICE (bro_makemkv_service_get_type ())
G_DECLARE_FINAL_TYPE (BroMakemkvService, bro_makemkv_service, BRO, MAKEMKV_SERVICE, GObject)

/* @data_dir: the user's MakeMKV data folder, for app_DataDir ("" on Windows: not set). @user_settings: the user's
 * settings.conf, where MakeMKV keeps its key (NULL on Windows). @home: the user's home folder, for a leading ~ in a
 * custom profile path or app_DataDir. */
BroMakemkvService *bro_makemkv_service_new (BroFileSystem *fs, BroConfigService *config, const char *data_dir, const char *user_settings,
                                            const char *home);

G_END_DECLS
