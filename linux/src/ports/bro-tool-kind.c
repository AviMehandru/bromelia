/* bro-tool-kind.c */
#include "bro-tool-kind.h"

#include <string.h>

static const char *const names[] = {
  "makemkvcon",
  "mkvmerge",
  "mkvextract",
  "ffmpeg",
  "tesseract",
  "handbrake",
  "cyanrip",
  "abcde",
  "par2",
  "apprise",
};

const char *
bro_tool_kind_to_wire (BroToolKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_tool_kind_from_wire (const char *text, BroToolKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroToolKind) i;
      return TRUE;
    }
  return FALSE;
}
