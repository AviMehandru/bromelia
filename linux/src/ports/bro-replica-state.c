/* bro-replica-state.c */
#include "bro-replica-state.h"

#include <string.h>

static const char *const names[] = {
  "pending",
  "copying",
  "verified",
  "stale",
  "failed",
};

const char *
bro_replica_state_to_wire (BroReplicaState value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_replica_state_from_wire (const char *text, BroReplicaState *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroReplicaState) i;
      return TRUE;
    }
  return FALSE;
}
