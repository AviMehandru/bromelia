/* bro-step-result.c */
#include "bro-step-result.h"

BroStepResult *
bro_step_result_new (BroStepKind kind, BroStepState state)
{
  BroStepResult *r = g_new0 (BroStepResult, 1);
  r->kind = kind;
  r->state = state;
  r->affects_outcome = TRUE;
  return r;
}

void
bro_step_result_free (BroStepResult *r)
{
  if (!r)
    return;
  bro_bro_error_free (r->error);
  bro_makemkv_notice_free (r->notice);
  bro_bro_message_free (r->skip);
  g_free (r);
}
