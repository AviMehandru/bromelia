/* bro-decision-answer.h: one answer of a decision: its id and its label. */
#pragma once

#include "bro-bro-message.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  BroBroMessage *label;
} BroDecisionAnswer;

/* Takes ownership of @label. */
BroDecisionAnswer *bro_decision_answer_new (const char *id, BroBroMessage *label);
BroDecisionAnswer *bro_decision_answer_copy (const BroDecisionAnswer *answer);
void bro_decision_answer_free (BroDecisionAnswer *answer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDecisionAnswer, bro_decision_answer_free)

G_END_DECLS
