/* bro-rip-mode.c */
#include "bro-rip-mode.h"

#include <string.h>

static const char *const names[] = {
  "mkv",
  "backup",
  "backupDecrypted",
  "backupThenMkv",
  "infoOnly",
  "audioCD",
  "dataImage",
};

const char *
bro_rip_mode_to_wire (BroRipMode value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_rip_mode_from_wire (const char *text, BroRipMode *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroRipMode) i;
      return TRUE;
    }
  return FALSE;
}
