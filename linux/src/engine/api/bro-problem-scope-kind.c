/* bro-problem-scope-kind.c */
#include "bro-problem-scope-kind.h"

#include <string.h>

static const char *const names[] = {
  "library",
  "drive",
  "job",
  "unit",
  "tool",
  "target",
  "notification",
  "secret",
};

const char *
bro_problem_scope_kind_to_wire (BroProblemScopeKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_problem_scope_kind_from_wire (const char *text, BroProblemScopeKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0)
      {
        *out = (BroProblemScopeKind) i;
        return TRUE;
      }
  return FALSE;
}
