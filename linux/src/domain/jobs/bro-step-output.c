/* bro-step-output.c */
#include "bro-step-output.h"

BroStepOutput *
bro_step_output_new (BroStepKind kind, BroJsonValue *data, BroBroMessage *summary)
{
  BroStepOutput *o = g_new0 (BroStepOutput, 1);
  o->kind = kind;
  o->data = data ? data : bro_json_value_new_null ();
  o->summary = summary;
  return o;
}

void
bro_step_output_free (BroStepOutput *output)
{
  if (!output)
    return;
  bro_json_value_unref (output->data);
  bro_bro_message_free (output->summary);
  g_free (output);
}
