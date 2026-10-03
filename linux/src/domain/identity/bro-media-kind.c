/* bro-media-kind.c */
#include "bro-media-kind.h"

#include <string.h>

static const char *const names[] = {
  "movie",
  "tv",
};

const char *
bro_media_kind_to_wire (BroMediaKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_media_kind_from_wire (const char *text, BroMediaKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroMediaKind) i;
      return TRUE;
    }
  return FALSE;
}
