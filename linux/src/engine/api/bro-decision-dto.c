/* bro-decision-dto.c */
#include "bro-decision-dto.h"

BroDecisionDto *
bro_decision_dto_new (const char *id, BroBroMessage *question)
{
  BroDecisionDto *d = g_new0 (BroDecisionDto, 1);
  d->id = g_strdup (id);
  d->question = question;
  d->answers = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_decision_answer_free);
  return d;
}

BroDecisionDto *
bro_decision_dto_copy (const BroDecisionDto *decision)
{
  BroDecisionDto *d;
  if (!decision)
    return NULL;
  d = bro_decision_dto_new (decision->id, bro_bro_message_copy (decision->question));
  for (guint i = 0; i < decision->answers->len; i++)
    g_ptr_array_add (d->answers, bro_decision_answer_copy (g_ptr_array_index (decision->answers, i)));
  return d;
}

void
bro_decision_dto_free (BroDecisionDto *decision)
{
  if (!decision)
    return;
  g_free (decision->id);
  bro_bro_message_free (decision->question);
  g_ptr_array_unref (decision->answers);
  g_free (decision);
}

BroJsonValue *
bro_decision_dto_json (const BroDecisionDto *decision)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *answers = bro_json_value_new_array ();
  for (guint i = 0; i < decision->answers->len; i++)
    {
      BroDecisionAnswer *a = g_ptr_array_index (decision->answers, i);
      BroJsonValue *j = bro_json_value_new_object ();
      bro_json_value_set (j, "id", bro_json_value_new_string (a->id));
      bro_json_value_set (j, "label", bro_bro_message_to_json (a->label));
      bro_json_value_append (answers, j);
    }
  bro_json_value_set (o, "id", bro_json_value_new_string (decision->id));
  bro_json_value_set (o, "question", bro_bro_message_to_json (decision->question));
  bro_json_value_set (o, "answers", answers);
  return o;
}
