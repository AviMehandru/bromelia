/* bro-power-manager.h: BroPowerManager: Keeps the computer awake. */
#pragma once

#include "bro-bro-error.h"
#include "bro-power-guard.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_POWER_MANAGER (bro_power_manager_get_type ())
G_DECLARE_INTERFACE (BroPowerManager, bro_power_manager, BRO, POWER_MANAGER, GObject)

struct _BroPowerManagerInterface {
  GTypeInterface parent_iface;

  BroPowerGuard *(*inhibit) (BroPowerManager *self, const char *reason, BroBroError **error);
};

/* Until the guard is released. */
BroPowerGuard *bro_power_manager_inhibit (BroPowerManager *self, const char *reason, BroBroError **error);

G_END_DECLS
