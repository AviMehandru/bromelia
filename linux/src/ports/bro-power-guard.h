/* bro-power-guard.h: BroPowerGuard: Held while the computer must stay awake. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_POWER_GUARD (bro_power_guard_get_type ())
G_DECLARE_INTERFACE (BroPowerGuard, bro_power_guard, BRO, POWER_GUARD, GObject)

struct _BroPowerGuardInterface {
  GTypeInterface parent_iface;

  void (*release) (BroPowerGuard *self);
};

void bro_power_guard_release (BroPowerGuard *self);

G_END_DECLS
