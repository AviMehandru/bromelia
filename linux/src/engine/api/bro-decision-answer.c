/* bro-decision-answer.c */
#include "bro-decision-answer.h"

BroDecisionAnswer *
bro_decision_answer_new (const char *id, BroBroMessage *label)
{
  BroDecisionAnswer *a = g_new0 (BroDecisionAnswer, 1);
  a->id = g_strdup (id);
  a->label = label;
  return a;
}

BroDecisionAnswer *
bro_decision_answer_copy (const BroDecisionAnswer *answer)
{
  return answer ? bro_decision_answer_new (answer->id, bro_bro_message_copy (answer->label)) : NULL;
}

void
bro_decision_answer_free (BroDecisionAnswer *answer)
{
  if (!answer)
    return;
  g_free (answer->id);
  bro_bro_message_free (answer->label);
  g_free (answer);
}
