/* bro-step-record.c */
#include "bro-step-record.h"

BroStepRecord *
bro_step_record_new (void)
{
  BroStepRecord *x = g_new0 (BroStepRecord, 1);
  return x;
}

void
bro_step_record_free (BroStepRecord *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->output, bro_step_output_free);
  g_clear_pointer (&x->error, bro_bro_error_free);
  g_clear_pointer (&x->checkpoint, bro_json_value_unref);
  g_free (x);
}

BroStepRecord *
bro_step_record_copy (const BroStepRecord *v)
{
  BroStepRecord *x;
  if (!v)
    return NULL;
  x = g_memdup2 (v, sizeof *v);
  x->output = bro_step_output_copy (v->output);
  x->error = bro_bro_error_copy (v->error);
  x->checkpoint = v->checkpoint ? bro_json_value_ref (v->checkpoint) : NULL;
  return x;
}
