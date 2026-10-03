/* bro-job-plan.h: the steps a job runs and what PlanStep decided (stored as jobs.plan): the mode, profile and
 * library, plus the rest in details. */
#pragma once

#include "bro-json-value.h"
#include "bro-step-kind.h"

G_BEGIN_DECLS

typedef struct {
  GArray *steps;         /* BroStepKind */
  char *mode;            /* nullable, a RipMode wire form */
  char *profile_id;      /* nullable */
  char *library_id;      /* nullable */
  BroJsonValue *details; /* nullable */
} BroJobPlan;

BroJobPlan *bro_job_plan_new (void);
void bro_job_plan_free (BroJobPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobPlan, bro_job_plan_free)

G_END_DECLS
