/* bro-job-planner.c */
#include "bro-job-planner.h"

static void
add (GArray *steps, BroStepKind kind)
{
  g_array_append_val (steps, kind);
}

GArray *
bro_job_planner_initial (const BroJobRequest *request)
{
  GArray *steps = g_array_new (FALSE, FALSE, sizeof (BroStepKind));
  if (request->kind != BRO_JOB_KIND_VIDEO_DISC)
    return steps;
  if (request->automatic && request->drive_id)
    add (steps, BRO_STEP_KIND_AWAIT_MEDIA);
  add (steps, BRO_STEP_KIND_PROBE);
  if (request->session_id)
    add (steps, BRO_STEP_KIND_RECONCILE);
  add (steps, BRO_STEP_KIND_IDENTIFY);
  add (steps, BRO_STEP_KIND_PLAN);
  return steps;
}
