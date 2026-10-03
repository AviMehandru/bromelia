/* bro-check-result.c */
#include "bro-check-result.h"

#include <string.h>

static const char *const names[] = {
  "ok",
  "damaged",
  "error",
  "stopped",
};

const char *
bro_check_result_to_wire (BroCheckResult value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_check_result_from_wire (const char *text, BroCheckResult *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroCheckResult) i;
      return TRUE;
    }
  return FALSE;
}
