/* bro-status-word.c */
#include "bro-status-word.h"

#include <string.h>

static const char *const names[] = {
  "success",
  "errors",
  "failed",
  "cancelled",
  "skipped",
  "interrupted",
};

const char *
bro_status_word_to_wire (BroStatusWord value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_status_word_from_wire (const char *text, BroStatusWord *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroStatusWord) i;
      return TRUE;
    }
  return FALSE;
}
