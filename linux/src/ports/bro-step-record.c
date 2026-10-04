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
