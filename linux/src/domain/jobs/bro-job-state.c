/* bro-job-state.c */
#include "bro-job-state.h"

#include <string.h>

static const char *const names[] = {
  "queued",
  "waitingForResources",
  "running",
  "blocked",
  "awaitingDecision",
  "finished",
};

const char *
bro_job_state_to_wire (BroJobState value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_job_state_from_wire (const char *text, BroJobState *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroJobState) i;
      return TRUE;
    }
  return FALSE;
}
