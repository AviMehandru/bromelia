/* bro-job-plan.c */
#include "bro-job-plan.h"

BroJobPlan *
bro_job_plan_new (void)
{
  BroJobPlan *p = g_new0 (BroJobPlan, 1);
  p->steps = g_array_new (FALSE, FALSE, sizeof (BroStepKind));
  return p;
}

void
bro_job_plan_free (BroJobPlan *plan)
{
  if (!plan)
    return;
  g_array_unref (plan->steps);
  g_free (plan->mode);
  g_free (plan->profile_id);
  g_free (plan->library_id);
  bro_json_value_unref (plan->details);
  g_free (plan);
}
