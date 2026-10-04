/* bro-device-event.c */
#include "bro-device-event.h"

BroDeviceEvent *
bro_device_event_new (void)
{
  BroDeviceEvent *x = g_new0 (BroDeviceEvent, 1);
  return x;
}

void
bro_device_event_free (BroDeviceEvent *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->drive, bro_os_drive_free);
  g_free (x->device);
  g_free (x->path);
  g_free (x);
}
