/* bro-resolve-layer.c */
#include "bro-resolve-layer.h"

#include <string.h>

static const char *const names[] = {
  "defaults",
  "defaultProfile",
  "driveProfile",
  "driveOverrides",
  "rule",
  "session",
};

const char *
bro_resolve_layer_to_wire (BroResolveLayer value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_resolve_layer_from_wire (const char *text, BroResolveLayer *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroResolveLayer) i;
      return TRUE;
    }
  return FALSE;
}
