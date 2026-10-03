/* bro-severity.c */
#include "bro-severity.h"

#include <string.h>

static const char *const names[] = {
  "debug",
  "info",
  "warning",
  "error",
};

const char *
bro_severity_to_wire (BroSeverity value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_severity_from_wire (const char *text, BroSeverity *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroSeverity) i;
      return TRUE;
    }
  return FALSE;
}
