/* bro-stop-reason.c */
#include "bro-stop-reason.h"

#include <string.h>

static const char *const names[] = {
  "cancelled",
  "stalled",
  "timedOut",
  "shutdown",
  "policy",
};

const char *
bro_stop_reason_to_wire (BroStopReason value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_stop_reason_from_wire (const char *text, BroStopReason *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroStopReason) i;
      return TRUE;
    }
  return FALSE;
}
