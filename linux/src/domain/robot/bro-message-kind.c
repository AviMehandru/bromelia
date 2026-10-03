/* bro-message-kind.c */
#include "bro-message-kind.h"

#include <string.h>

static const char *const names[] = {
  "debug",
  "info",
  "warning",
  "error",
};

const char *
bro_message_kind_to_wire (BroMessageKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_message_kind_from_wire (const char *text, BroMessageKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroMessageKind) i;
      return TRUE;
    }
  return FALSE;
}
