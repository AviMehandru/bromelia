/* bro-platform-device-monitor.h: BroPlatformDeviceMonitor: DeviceMonitor on Linux (plan §10.3), polled: the sr*
 * drives in sysfs (device /dev/srN, identification "vendor model"), media from the CDROM_DRIVE_STATUS ioctl (it never
 * moves the tray), the mount from /proc/self/mountinfo. A BroDrivePoller turns changes into events. Built on macOS (for
 * the tests) it finds no drives. */
#pragma once

#include "bro-clock.h"
#include "bro-device-monitor.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLATFORM_DEVICE_MONITOR (bro_platform_device_monitor_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformDeviceMonitor, bro_platform_device_monitor, BRO, PLATFORM_DEVICE_MONITOR, GObject)

/* @interval_seconds: how often to look (2 when 0). Keeps a reference to @clock. */
BroPlatformDeviceMonitor *bro_platform_device_monitor_new (BroClock *clock, double interval_seconds);

void bro_platform_device_monitor_start (BroPlatformDeviceMonitor *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy);
void bro_platform_device_monitor_stop (BroPlatformDeviceMonitor *self);
/* BroOsDrive *. */
GPtrArray *bro_platform_device_monitor_current_drives (BroPlatformDeviceMonitor *self);
/* The optical drives now (BroOsDriveState *), by device. */
GPtrArray *bro_platform_device_monitor_snapshot (BroPlatformDeviceMonitor *self);

G_END_DECLS
