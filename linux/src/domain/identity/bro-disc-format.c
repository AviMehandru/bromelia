/* bro-disc-format.c */
#include "bro-disc-format.h"

#include <string.h>

static const char *const names[] = {
  "dvd",
  "bluray",
  "uhd",
  "hddvd",
  "unknown",
};

const char *
bro_disc_format_to_wire (BroDiscFormat value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_disc_format_from_wire (const char *text, BroDiscFormat *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroDiscFormat) i;
      return TRUE;
    }
  return FALSE;
}
