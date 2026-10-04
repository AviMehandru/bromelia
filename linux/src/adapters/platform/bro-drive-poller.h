/* bro-drive-poller.h: BroDrivePoller: a DeviceMonitor that looks at the drives every interval and reports what changed
 * (bro_os_drive_state_changes; shared/fixtures/adapters/os-drive-states.cases.json). The platform monitor is this over
 * the system's snapshot. */
#pragma once

#include "bro-clock.h"
#include "bro-device-monitor.h"
#include "bro-os-drive-state.h"
#include <glib-object.h>

G_BEGIN_DECLS

/* The drives now (BroOsDriveState *), in a stable order. */
typedef GPtrArray *(*BroOsDriveSnapshotFunc) (gpointer data);

#define BRO_TYPE_DRIVE_POLLER (bro_drive_poller_get_type ())
G_DECLARE_FINAL_TYPE (BroDrivePoller, bro_drive_poller, BRO, DRIVE_POLLER, GObject)

/* Keeps a reference to @clock; @destroy frees @data with the poller. */
BroDrivePoller *bro_drive_poller_new (BroOsDriveSnapshotFunc snapshot, gpointer data, GDestroyNotify destroy, BroClock *clock, BroDuration interval);

/* Takes a snapshot now (no events for what is already there), then one each interval; @sink is called on the clock's
 * thread, outside the poller's lock. */
void bro_drive_poller_start (BroDrivePoller *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy);
void bro_drive_poller_stop (BroDrivePoller *self);
/* BroOsDrive *: a fresh snapshot. */
GPtrArray *bro_drive_poller_current_drives (BroDrivePoller *self);

G_END_DECLS
