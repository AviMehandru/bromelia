/* bro-libre-drive-state.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_LIBRE_DRIVE_STATE_ENABLED,
  BRO_LIBRE_DRIVE_STATE_NOT_IN_USE,
  BRO_LIBRE_DRIVE_STATE_REQUIRED,
} BroLibreDriveState;

/* The wire form ("enabled"), as in shared/schema/common.json. */
const char *bro_libre_drive_state_to_wire (BroLibreDriveState value);
gboolean bro_libre_drive_state_from_wire (const char *text, BroLibreDriveState *out);

G_END_DECLS
