/* bro-unit-status.c */
#include "bro-unit-status.h"

#include <string.h>

static const char *const names[] = {
  "success",
  "errors",
  "incomplete",
};

const char *
bro_unit_status_to_wire (BroUnitStatus value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_unit_status_from_wire (const char *text, BroUnitStatus *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroUnitStatus) i;
      return TRUE;
    }
  return FALSE;
}
