/* bro-conflict-policy.c */
#include "bro-conflict-policy.h"

#include <string.h>

static const char *const names[] = {
  "newFolder",
  "merge",
  "skip",
};

const char *
bro_conflict_policy_to_wire (BroConflictPolicy value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_conflict_policy_from_wire (const char *text, BroConflictPolicy *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroConflictPolicy) i;
      return TRUE;
    }
  return FALSE;
}
