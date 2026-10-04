/* bro-move-policy.c */
#include "bro-move-policy.h"

#include <string.h>

static const char *const names[] = {
  "neverReplace",
};

const char *
bro_move_policy_to_wire (BroMovePolicy value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_move_policy_from_wire (const char *text, BroMovePolicy *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroMovePolicy) i;
      return TRUE;
    }
  return FALSE;
}
