/* bro-layout.c */
#include "bro-layout.h"

#include <string.h>

static const char *const names[] = {
  "templates",
  "mediaServer",
};

const char *
bro_layout_to_wire (BroLayout value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_layout_from_wire (const char *text, BroLayout *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroLayout) i;
      return TRUE;
    }
  return FALSE;
}
