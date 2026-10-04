/* bro-power-guard.c */
#include "bro-power-guard.h"

G_DEFINE_INTERFACE (BroPowerGuard, bro_power_guard, G_TYPE_OBJECT)

static void
bro_power_guard_default_init (BroPowerGuardInterface *iface)
{
}

void
bro_power_guard_release (BroPowerGuard *self)
{
  g_return_if_fail (BRO_IS_POWER_GUARD (self));
  BRO_POWER_GUARD_GET_IFACE (self)->release (self);
}
