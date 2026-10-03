/* bro-file-verdict.c */
#include "bro-file-verdict.h"

#include <string.h>

static const char *const names[] = {
  "same",
  "changed",
  "missing",
  "unreadable",
};

const char *
bro_file_verdict_to_wire (BroFileVerdict value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_file_verdict_from_wire (const char *text, BroFileVerdict *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroFileVerdict) i;
      return TRUE;
    }
  return FALSE;
}
