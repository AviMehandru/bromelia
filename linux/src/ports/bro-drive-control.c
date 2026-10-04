/* bro-drive-control.c */
#include "bro-drive-control.h"

G_DEFINE_INTERFACE (BroDriveControl, bro_drive_control, G_TYPE_OBJECT)

static void
bro_drive_control_default_init (BroDriveControlInterface *iface)
{
}

gboolean
bro_drive_control_eject (BroDriveControl *self, const char *device, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_DRIVE_CONTROL (self), FALSE);
  return BRO_DRIVE_CONTROL_GET_IFACE (self)->eject (self, device, error);
}

gboolean
bro_drive_control_close_tray (BroDriveControl *self, const char *device, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_DRIVE_CONTROL (self), FALSE);
  return BRO_DRIVE_CONTROL_GET_IFACE (self)->close_tray (self, device, error);
}

char *
bro_drive_control_wait_for_mount (BroDriveControl *self, const char *device, BroDuration timeout, BroCancellationToken *cancel)
{
  g_return_val_if_fail (BRO_IS_DRIVE_CONTROL (self), NULL);
  return BRO_DRIVE_CONTROL_GET_IFACE (self)->wait_for_mount (self, device, timeout, cancel);
}

BroDiscContent
bro_drive_control_probe_content (BroDriveControl *self, const char *device)
{
  g_return_val_if_fail (BRO_IS_DRIVE_CONTROL (self), 0);
  return BRO_DRIVE_CONTROL_GET_IFACE (self)->probe_content (self, device);
}

BroSectorReader *
bro_drive_control_open_raw (BroDriveControl *self, const char *device, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_DRIVE_CONTROL (self), NULL);
  return BRO_DRIVE_CONTROL_GET_IFACE (self)->open_raw (self, device, error);
}
