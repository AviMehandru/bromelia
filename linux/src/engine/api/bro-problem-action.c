/* bro-problem-action.c */
#include "bro-problem-action.h"

#include <string.h>

static const char *const names[] = {
  "installBetaKey",
  "openSettings",
  "attachLibrary",
  "repair",
  "retry",
};

const char *
bro_problem_action_to_wire (BroProblemAction value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_problem_action_from_wire (const char *text, BroProblemAction *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0)
      {
        *out = (BroProblemAction) i;
        return TRUE;
      }
  return FALSE;
}
