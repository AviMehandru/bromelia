/* bro-decision-dto.h: a question a job waits on (dtos.json Decision). */
#pragma once

#include "bro-decision-answer.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  BroBroMessage *question;
  GPtrArray *answers; /* BroDecisionAnswer * */
} BroDecisionDto;

/* Takes ownership of @question; no answers yet. */
BroDecisionDto *bro_decision_dto_new (const char *id, BroBroMessage *question);
BroDecisionDto *bro_decision_dto_copy (const BroDecisionDto *decision);
void bro_decision_dto_free (BroDecisionDto *decision);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_decision_dto_json (const BroDecisionDto *decision);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDecisionDto, bro_decision_dto_free)

G_END_DECLS
