/* bro-stop-policy.c */
#include "bro-stop-policy.h"

#include <string.h>

static const char *const names[] = {
  "terminateFirst",
  "interruptFirst",
};

const char *
bro_stop_policy_to_wire (BroStopPolicy value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_stop_policy_from_wire (const char *text, BroStopPolicy *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroStopPolicy) i;
      return TRUE;
    }
  return FALSE;
}
