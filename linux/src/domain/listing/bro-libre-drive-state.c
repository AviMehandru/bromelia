/* bro-libre-drive-state.c */
#include "bro-libre-drive-state.h"

#include <string.h>

static const char *const names[] = {
  "enabled",
  "notInUse",
  "required",
};

const char *
bro_libre_drive_state_to_wire (BroLibreDriveState value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_libre_drive_state_from_wire (const char *text, BroLibreDriveState *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroLibreDriveState) i;
      return TRUE;
    }
  return FALSE;
}
