/* bro-drive-status.c */
#include "bro-drive-status.h"

#include <string.h>

static const char *const names[] = {
  "empty",
  "trayOpen",
  "loading",
  "present",
  "unavailable",
  "unmounting",
};

const char *
bro_drive_status_to_wire (BroDriveStatus value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_drive_status_from_wire (const char *text, BroDriveStatus *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0)
      {
        *out = (BroDriveStatus) i;
        return TRUE;
      }
  return FALSE;
}
