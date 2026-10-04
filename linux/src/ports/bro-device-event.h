/* bro-device-event.h: what the operating system reported about a drive. */
#pragma once

#include "bro-os-drive.h"
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_DEVICE_EVENT_DRIVE_APPEARED, /* drive */
  BRO_DEVICE_EVENT_DRIVE_VANISHED, /* device */
  BRO_DEVICE_EVENT_MEDIA_ARRIVED, /* device */
  BRO_DEVICE_EVENT_MEDIA_REMOVED, /* device */
  BRO_DEVICE_EVENT_TRAY_OPENED, /* device */
  BRO_DEVICE_EVENT_MOUNTED, /* device, path */
  BRO_DEVICE_EVENT_UNMOUNTED, /* device */
} BroDeviceEventKind;

typedef struct {
  BroDeviceEventKind kind;
  BroOsDrive *drive;
  char *device;
  char *path;
} BroDeviceEvent;

/* Everything zero. */
BroDeviceEvent *bro_device_event_new (void);
void bro_device_event_free (BroDeviceEvent *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDeviceEvent, bro_device_event_free)

G_END_DECLS
