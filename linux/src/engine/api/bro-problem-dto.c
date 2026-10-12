/* bro-problem-dto.c */
#include "bro-problem-dto.h"

BroProblemDto *
bro_problem_dto_new (const char *id, const BroBroMessage *message, const BroProblemScope *scope, BroInstant raised_at,
                     const BroProblemAction *actions, gsize n_actions)
{
  BroProblemDto *p = g_new0 (BroProblemDto, 1);
  p->id = g_strdup (id);
  p->message = bro_bro_message_copy (message);
  p->scope = bro_problem_scope_copy (scope);
  p->raised_at = raised_at;
  p->actions = g_array_new (FALSE, FALSE, sizeof (BroProblemAction));
  if (n_actions > 0)
    g_array_append_vals (p->actions, actions, n_actions);
  return p;
}

BroProblemDto *
bro_problem_dto_copy (const BroProblemDto *problem)
{
  if (!problem)
    return NULL;
  return bro_problem_dto_new (problem->id, problem->message, problem->scope, problem->raised_at,
                              (const BroProblemAction *) problem->actions->data, problem->actions->len);
}

void
bro_problem_dto_free (BroProblemDto *problem)
{
  if (!problem)
    return;
  g_free (problem->id);
  bro_bro_message_free (problem->message);
  bro_problem_scope_free (problem->scope);
  g_array_unref (problem->actions);
  g_free (problem);
}

BroJsonValue *
bro_problem_dto_json (const BroProblemDto *problem)
{
  BroJsonValue *o = bro_json_value_new_object ();
  g_autofree char *raised = bro_instant_format (problem->raised_at);
  bro_json_value_set (o, "id", bro_json_value_new_string (problem->id));
  bro_json_value_set (o, "code", bro_json_value_new_string (bro_message_code_wire (problem->message->code)));
  if (bro_json_value_length (problem->message->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (problem->message->params));
  bro_json_value_set (o, "severity", bro_json_value_new_string (bro_severity_to_wire (problem->message->severity)));
  if (problem->scope)
    {
      BroJsonValue *s = bro_json_value_new_object ();
      bro_json_value_set (s, "kind", bro_json_value_new_string (bro_problem_scope_kind_to_wire (problem->scope->kind)));
      bro_json_value_set (s, "id", bro_json_value_new_string (problem->scope->id));
      bro_json_value_set (o, "scope", s);
    }
  bro_json_value_set (o, "raisedAt", bro_json_value_new_string (raised));
  if (problem->actions->len > 0)
    {
      BroJsonValue *a = bro_json_value_new_array ();
      for (guint i = 0; i < problem->actions->len; i++)
        bro_json_value_append (a, bro_json_value_new_string (bro_problem_action_to_wire (g_array_index (problem->actions, BroProblemAction, i))));
      bro_json_value_set (o, "actions", a);
    }
  return o;
}
