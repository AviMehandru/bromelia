/* bro-problem-scope.h: the thing a problem is about: a library, a drive, a job…, by its id. */
#pragma once

#include "bro-problem-scope-kind.h"

G_BEGIN_DECLS

typedef struct {
  BroProblemScopeKind kind;
  char *id;
} BroProblemScope;

BroProblemScope *bro_problem_scope_new (BroProblemScopeKind kind, const char *id);
BroProblemScope *bro_problem_scope_copy (const BroProblemScope *scope);
void bro_problem_scope_free (BroProblemScope *scope);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroProblemScope, bro_problem_scope_free)

G_END_DECLS
