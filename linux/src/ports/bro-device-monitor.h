/* bro-device-monitor.h: BroDeviceMonitor: Optical drives as the operating system sees them. */
#pragma once

#include "bro-device-event.h"
#include "bro-os-drive.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_DEVICE_MONITOR (bro_device_monitor_get_type ())
G_DECLARE_INTERFACE (BroDeviceMonitor, bro_device_monitor, BRO, DEVICE_MONITOR, GObject)

typedef void (*BroDeviceSinkFunc) (const BroDeviceEvent *event, gpointer data);

struct _BroDeviceMonitorInterface {
  GTypeInterface parent_iface;

  void (*start) (BroDeviceMonitor *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy);
  void (*stop) (BroDeviceMonitor *self);
  GPtrArray *(*current_drives) (BroDeviceMonitor *self);
};

/* Reports events to sink (on the monitor's thread) until stop. */
void bro_device_monitor_start (BroDeviceMonitor *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy);

void bro_device_monitor_stop (BroDeviceMonitor *self);

GPtrArray *bro_device_monitor_current_drives (BroDeviceMonitor *self);

G_END_DECLS
