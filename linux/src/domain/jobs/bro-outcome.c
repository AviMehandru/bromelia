/* bro-outcome.c */
#include "bro-outcome.h"

#include <string.h>

static const char *const names[] = {
  "succeeded",
  "succeededWithReadErrors",
  "succeededStepsFailed",
  "failed",
  "cancelled",
  "skipped",
  "interrupted",
};

const char *
bro_outcome_to_wire (BroOutcome value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_outcome_from_wire (const char *text, BroOutcome *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroOutcome) i;
      return TRUE;
    }
  return FALSE;
}
