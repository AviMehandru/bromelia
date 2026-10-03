/* bro-disc-type.c */
#include "bro-disc-type.h"

#include <string.h>

static const char *const names[] = {
  "dvd",
  "bd",
  "hddvd",
  "disc",
};

const char *
bro_disc_type_to_wire (BroDiscType value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_disc_type_from_wire (const char *text, BroDiscType *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroDiscType) i;
      return TRUE;
    }
  return FALSE;
}
