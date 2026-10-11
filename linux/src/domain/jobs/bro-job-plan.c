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

BroJobPlan *
bro_job_plan_copy (const BroJobPlan *plan)
{
  BroJobPlan *p;
  if (!plan)
    return NULL;
  p = bro_job_plan_new ();
  g_array_append_vals (p->steps, plan->steps->data, plan->steps->len);
  p->mode = g_strdup (plan->mode);
  p->profile_id = g_strdup (plan->profile_id);
  p->library_id = g_strdup (plan->library_id);
  p->details = plan->details ? bro_json_value_ref (plan->details) : NULL;
  return p;
}
