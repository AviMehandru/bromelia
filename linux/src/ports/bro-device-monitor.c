/* bro-device-monitor.c */
#include "bro-device-monitor.h"

G_DEFINE_INTERFACE (BroDeviceMonitor, bro_device_monitor, G_TYPE_OBJECT)

static void
bro_device_monitor_default_init (BroDeviceMonitorInterface *iface)
{
}

void
bro_device_monitor_start (BroDeviceMonitor *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_DEVICE_MONITOR (self));
  BRO_DEVICE_MONITOR_GET_IFACE (self)->start (self, sink, data, destroy);
}

void
bro_device_monitor_stop (BroDeviceMonitor *self)
{
  g_return_if_fail (BRO_IS_DEVICE_MONITOR (self));
  BRO_DEVICE_MONITOR_GET_IFACE (self)->stop (self);
}

GPtrArray *
bro_device_monitor_current_drives (BroDeviceMonitor *self)
{
  g_return_val_if_fail (BRO_IS_DEVICE_MONITOR (self), NULL);
  return BRO_DEVICE_MONITOR_GET_IFACE (self)->current_drives (self);
}
