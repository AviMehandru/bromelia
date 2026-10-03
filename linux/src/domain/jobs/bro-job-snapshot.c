/* bro-job-snapshot.c */
#include "bro-job-snapshot.h"

BroJobSnapshot *
bro_job_snapshot_new (const BroId *id, BroJobRequest *request, BroJobPlan *plan)
{
  BroJobSnapshot *s = g_new0 (BroJobSnapshot, 1);
  s->id = *id;
  s->request = request;
  s->plan = plan;
  s->outputs = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_output_free);
  return s;
}

void
bro_job_snapshot_free (BroJobSnapshot *snapshot)
{
  if (!snapshot)
    return;
  bro_job_request_free (snapshot->request);
  bro_job_plan_free (snapshot->plan);
  g_ptr_array_unref (snapshot->outputs);
  g_free (snapshot);
}
