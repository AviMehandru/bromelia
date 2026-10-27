/* bro-job-step-dto.c */
#include "bro-job-step-dto.h"

BroJobStepDto *
bro_job_step_dto_new (int seq, BroStepKind kind, BroStepState state)
{
  BroJobStepDto *s = g_new0 (BroJobStepDto, 1);
  s->seq = seq;
  s->kind = kind;
  s->state = state;
  return s;
}

void
bro_job_step_dto_free (BroJobStepDto *step)
{
  if (!step)
    return;
  bro_bro_error_free (step->error);
  bro_bro_message_free (step->summary);
  g_free (step);
}
