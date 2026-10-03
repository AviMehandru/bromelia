/* bro-path-role.c */
#include "bro-path-role.h"

#include <string.h>

static const char *const names[] = {
  "title",
  "episode",
  "closingClip",
  "playAll",
  "backup",
  "image",
  "audio",
  "discInfo",
  "record",
  "sums",
  "log",
  "nfo",
  "poster",
};

const char *
bro_path_role_to_wire (BroPathRole value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_path_role_from_wire (const char *text, BroPathRole *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroPathRole) i;
      return TRUE;
    }
  return FALSE;
}
