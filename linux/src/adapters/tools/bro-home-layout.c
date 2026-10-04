/* bro-home-layout.c */
#include "bro-home-layout.h"

#include <string.h>

static const char *const names[] = {
  "macos",
  "linux",
};

const char *
bro_home_layout_to_wire (BroHomeLayout value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_home_layout_from_wire (const char *text, BroHomeLayout *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroHomeLayout) i;
      return TRUE;
    }
  return FALSE;
}
