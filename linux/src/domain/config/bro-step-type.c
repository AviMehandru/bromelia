/* bro-step-type.c */
#include "bro-step-type.h"

#include <string.h>

static const char *const names[] = {
  "command",
  "handbrake",
};

const char *
bro_step_type_to_wire (BroStepType value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_step_type_from_wire (const char *text, BroStepType *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroStepType) i;
      return TRUE;
    }
  return FALSE;
}
