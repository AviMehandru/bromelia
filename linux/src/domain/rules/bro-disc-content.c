/* bro-disc-content.c */
#include "bro-disc-content.h"

#include <string.h>

static const char *const names[] = {
  "video",
  "audio",
  "data",
  "blank",
  "unknown",
};

const char *
bro_disc_content_to_wire (BroDiscContent value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_disc_content_from_wire (const char *text, BroDiscContent *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroDiscContent) i;
      return TRUE;
    }
  return FALSE;
}
