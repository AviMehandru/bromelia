/* bro-unit-state.c */
#include "bro-unit-state.h"

#include <string.h>

static const char *const names[] = {
  "committing",
  "committed",
  "quarantined",
  "missing",
};

const char *
bro_unit_state_to_wire (BroUnitState value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_unit_state_from_wire (const char *text, BroUnitState *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroUnitState) i;
      return TRUE;
    }
  return FALSE;
}
