/* bro-settings-isolation.c */
#include "bro-settings-isolation.h"

G_DEFINE_INTERFACE (BroSettingsIsolation, bro_settings_isolation, G_TYPE_OBJECT)

static void
bro_settings_isolation_default_init (BroSettingsIsolationInterface *iface)
{
}

BroIsolationLease *
bro_settings_isolation_prepare (BroSettingsIsolation *self, const BroMakemkvRunSettings *settings, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_SETTINGS_ISOLATION (self), NULL);
  return BRO_SETTINGS_ISOLATION_GET_IFACE (self)->prepare (self, settings, error);
}
