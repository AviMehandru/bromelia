/* bro-drive-state.h: a drive's state in a DRV line (MakeMKV's numbers are 0, 1, 2, 3, 256 and 257) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_DRIVE_STATE_EMPTY_CLOSED,
  BRO_DRIVE_STATE_EMPTY_OPEN,
  BRO_DRIVE_STATE_INSERTED,
  BRO_DRIVE_STATE_LOADING,
  BRO_DRIVE_STATE_NO_DRIVE,
  BRO_DRIVE_STATE_UNMOUNTING,
} BroDriveState;

/* The wire form ("emptyClosed"), as in shared/schema/common.json. */
const char *bro_drive_state_to_wire (BroDriveState value);
gboolean bro_drive_state_from_wire (const char *text, BroDriveState *out);

G_END_DECLS
