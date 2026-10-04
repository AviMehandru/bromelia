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
};

BroIsolationLease *bro_settings_isolation_prepare (BroSettingsIsolation *self, const BroMakemkvRunSettings *settings, BroBroError **error);

G_END_DECLS
