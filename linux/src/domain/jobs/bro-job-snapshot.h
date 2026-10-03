/* bro-job-snapshot.h: an immutable view of a job that a step gets in its context (plan §12.2): its request, its
 * plan and the outputs of the steps before it. */
#pragma once

#include "bro-id.h"
#include "bro-job-plan.h"
#include "bro-job-request.h"
#include "bro-step-output.h"

G_BEGIN_DECLS

typedef struct {
  BroId id;
  BroJobRequest *request;
  BroJobPlan *plan;   /* nullable */
  GPtrArray *outputs; /* BroStepOutput * */
} BroJobSnapshot;

/* Takes ownership of @request and @plan. */
BroJobSnapshot *bro_job_snapshot_new (const BroId *id, BroJobRequest *request, BroJobPlan *plan);
void bro_job_snapshot_free (BroJobSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobSnapshot, bro_job_snapshot_free)

G_END_DECLS
