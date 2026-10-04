/* bro-platform-power-manager.h: BroPlatformPowerManager: PowerManager on Linux (plan §10.3): a systemd-logind inhibitor per
 * guard, held by its file descriptor. It asks for sleep:idle; logind refuses sleep to a process outside a local desktop
 * session (a systemd --user service, SSH), so it then takes idle alone, which still stops idle suspend. power.unavailable
 * when there is no logind (Docker, no system bus) or it refuses both. */
#pragma once

#include "bro-power-manager.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLATFORM_POWER_MANAGER (bro_platform_power_manager_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformPowerManager, bro_platform_power_manager, BRO, PLATFORM_POWER_MANAGER, GObject)

BroPlatformPowerManager *bro_platform_power_manager_new (void);

/* NULL and @error set (power.unavailable) on failure. */
BroPowerGuard *bro_platform_power_manager_inhibit (BroPlatformPowerManager *self, const char *reason, BroBroError **error);

G_END_DECLS
