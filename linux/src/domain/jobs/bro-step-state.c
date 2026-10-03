/* bro-step-state.c */
#include "bro-step-state.h"

#include <string.h>

static const char *const names[] = {
  "pending",
  "running",
  "succeeded",
  "failed",
  "skipped",
  "cancelled",
  "interrupted",
};

const char *
bro_step_state_to_wire (BroStepState value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_step_state_from_wire (const char *text, BroStepState *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroStepState) i;
      return TRUE;
    }
  return FALSE;
}
