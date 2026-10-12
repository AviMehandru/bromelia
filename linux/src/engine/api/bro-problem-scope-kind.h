/* bro-problem-scope-kind.h: what a problem is about (dtos.json Problem.scope.kind). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_PROBLEM_SCOPE_KIND_LIBRARY,
  BRO_PROBLEM_SCOPE_KIND_DRIVE,
  BRO_PROBLEM_SCOPE_KIND_JOB,
  BRO_PROBLEM_SCOPE_KIND_UNIT,
  BRO_PROBLEM_SCOPE_KIND_TOOL,
  BRO_PROBLEM_SCOPE_KIND_TARGET,
  BRO_PROBLEM_SCOPE_KIND_NOTIFICATION,
  BRO_PROBLEM_SCOPE_KIND_SECRET,
} BroProblemScopeKind;

/* The wire form ("library"), as in shared/schema/api/dtos.json. */
const char *bro_problem_scope_kind_to_wire (BroProblemScopeKind value);
gboolean bro_problem_scope_kind_from_wire (const char *text, BroProblemScopeKind *out);

G_END_DECLS
