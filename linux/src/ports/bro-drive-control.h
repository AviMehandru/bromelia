/* bro-drive-control.h: BroDriveControl: Trays, mounts and raw access. */
#pragma once

#include "bro-bro-error.h"
#include "bro-cancellation-token.h"
#include "bro-disc-content.h"
#include "bro-duration.h"
#include "bro-sector-reader.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_DRIVE_CONTROL (bro_drive_control_get_type ())
G_DECLARE_INTERFACE (BroDriveControl, bro_drive_control, BRO, DRIVE_CONTROL, GObject)

struct _BroDriveControlInterface {
  GTypeInterface parent_iface;

  gboolean (*eject) (BroDriveControl *self, const char *device, BroBroError **error);
  gboolean (*close_tray) (BroDriveControl *self, const char *device, BroBroError **error);
  char *(*wait_for_mount) (BroDriveControl *self, const char *device, BroDuration timeout, BroCancellationToken *cancel);
  BroDiscContent (*probe_content) (BroDriveControl *self, const char *device);
  BroSectorReader *(*open_raw) (BroDriveControl *self, const char *device, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_drive_control_eject (BroDriveControl *self, const char *device, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_drive_control_close_tray (BroDriveControl *self, const char *device, BroBroError **error);

/* The mount path; none after timeout or when cancelled. Free with g_free. NULL when there is none. */
char *bro_drive_control_wait_for_mount (BroDriveControl *self, const char *device, BroDuration timeout, BroCancellationToken *cancel);

BroDiscContent bro_drive_control_probe_content (BroDriveControl *self, const char *device);

BroSectorReader *bro_drive_control_open_raw (BroDriveControl *self, const char *device, BroBroError **error);

G_END_DECLS
