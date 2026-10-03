/* bro-run-on.c */
#include "bro-run-on.h"

#include <string.h>

static const char *const names[] = {
  "success",
  "failure",
  "always",
};

const char *
bro_run_on_to_wire (BroRunOn value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_run_on_from_wire (const char *text, BroRunOn *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroRunOn) i;
      return TRUE;
    }
  return FALSE;
}
