/* bro-settings-isolation.h: BroSettingsIsolation: Gives each makemkvcon run its own settings. */
#pragma once

#include "bro-bro-error.h"
#include "bro-isolation-lease.h"
#include "bro-makemkv-run-settings.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SETTINGS_ISOLATION (bro_settings_isolation_get_type ())
G_DECLARE_INTERFACE (BroSettingsIsolation, bro_settings_isolation, BRO, SETTINGS_ISOLATION, GObject)

struct _BroSettingsIsolationInterface {
  GTypeInterface parent_iface;

  BroIsolationLease *(*prepare) (BroSettingsIsolation *self, const BroMakemkvRunSettings *settings, BroBroError **error);
  GPtrArray *(*recover) (BroSettingsIsolation *self, GPtrArray *work_directories);
};

BroIsolationLease *bro_settings_isolation_prepare (BroSettingsIsolation *self, const BroMakemkvRunSettings *settings, BroBroError **error);

/* At startup, for the runs the engine never released (it stopped while they ran): a HOME folder loses the registration
 * key line of its settings.conf; the registry gets back the user's values a crash left in the snapshot file.
 * @work_directories (char *) are those runs' folders (the unfinished jobs' home/). What it couldn't do, as warnings
 * (makemkv.keyNotRemoved, makemkv.registryNotRestored, makemkv.registryFailed): a new array of BroBroMessage that frees
 * them, empty when all went well. */
GPtrArray *bro_settings_isolation_recover (BroSettingsIsolation *self, GPtrArray *work_directories);

G_END_DECLS
