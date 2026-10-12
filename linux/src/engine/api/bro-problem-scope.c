/* bro-problem-scope.c */
#include "bro-problem-scope.h"

BroProblemScope *
bro_problem_scope_new (BroProblemScopeKind kind, const char *id)
{
  BroProblemScope *s = g_new0 (BroProblemScope, 1);
  s->kind = kind;
  s->id = g_strdup (id);
  return s;
}

BroProblemScope *
bro_problem_scope_copy (const BroProblemScope *scope)
{
  return scope ? bro_problem_scope_new (scope->kind, scope->id) : NULL;
}

void
bro_problem_scope_free (BroProblemScope *scope)
{
  if (!scope)
    return;
  g_free (scope->id);
  g_free (scope);
}
