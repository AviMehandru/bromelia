/* bro-drive-state.c */
#include "bro-drive-state.h"

#include <string.h>

static const char *const names[] = {
  "emptyClosed",
  "emptyOpen",
  "inserted",
  "loading",
  "noDrive",
  "unmounting",
};

const char *
bro_drive_state_to_wire (BroDriveState value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_drive_state_from_wire (const char *text, BroDriveState *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroDriveState) i;
      return TRUE;
    }
  return FALSE;
}
