/* bro-drive-status.h: BroDriveStatus: a drive's state as clients see it (dtos.json Drive.state): MakeMKV's state of a
 * connected drive, or unavailable for a configured drive that isn't connected. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_DRIVE_STATUS_EMPTY,
  BRO_DRIVE_STATUS_TRAY_OPEN,
  BRO_DRIVE_STATUS_LOADING,
  BRO_DRIVE_STATUS_PRESENT,
  BRO_DRIVE_STATUS_UNAVAILABLE,
  BRO_DRIVE_STATUS_UNMOUNTING,
} BroDriveStatus;

/* The wire form ("trayOpen"), as in shared/schema/api/dtos.json. */
const char *bro_drive_status_to_wire (BroDriveStatus value);
gboolean bro_drive_status_from_wire (const char *text, BroDriveStatus *out);

G_END_DECLS
