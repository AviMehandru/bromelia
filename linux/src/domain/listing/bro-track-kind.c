/* bro-track-kind.c */
#include "bro-track-kind.h"

#include <string.h>

static const char *const names[] = {
  "video",
  "audio",
  "subtitle",
  "attachment",
  "unknown",
};

const char *
bro_track_kind_to_wire (BroTrackKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_track_kind_from_wire (const char *text, BroTrackKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroTrackKind) i;
      return TRUE;
    }
  return FALSE;
}
