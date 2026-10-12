/* bro-problem-dto.h: a problem every UI shows until it is cleared (dtos.json Problem): the message (code, params,
 * severity warning or error), what it is about, when it was first raised and what a UI can offer. */
#pragma once

#include "bro-bro-message.h"
#include "bro-instant.h"
#include "bro-problem-action.h"
#include "bro-problem-scope.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  BroBroMessage *message;
  BroProblemScope *scope; /* NULL: none */
  BroInstant raised_at;
  GArray *actions; /* BroProblemAction */
} BroProblemDto;

/* Copies @message, @scope (may be NULL) and @n_actions @actions. */
BroProblemDto *bro_problem_dto_new (const char *id, const BroBroMessage *message, const BroProblemScope *scope, BroInstant raised_at,
                                    const BroProblemAction *actions, gsize n_actions);
BroProblemDto *bro_problem_dto_copy (const BroProblemDto *problem);
void bro_problem_dto_free (BroProblemDto *problem);

/* Optional members left out when absent or empty. Free with bro_json_value_unref. */
BroJsonValue *bro_problem_dto_json (const BroProblemDto *problem);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroProblemDto, bro_problem_dto_free)

G_END_DECLS
