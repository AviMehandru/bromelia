/* bro-power-manager.c */
#include "bro-power-manager.h"

G_DEFINE_INTERFACE (BroPowerManager, bro_power_manager, G_TYPE_OBJECT)

static void
bro_power_manager_default_init (BroPowerManagerInterface *iface)
{
}

BroPowerGuard *
bro_power_manager_inhibit (BroPowerManager *self, const char *reason, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_POWER_MANAGER (self), NULL);
  return BRO_POWER_MANAGER_GET_IFACE (self)->inhibit (self, reason, error);
}
