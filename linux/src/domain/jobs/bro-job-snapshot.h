/* bro-job-snapshot.h: BroJobSnapshot, an immutable view of a job that a step gets in its context (plan §12.2): its
 * request, its plan, the outputs of the steps before it, and when the job first started running (none before its first
 * step; the archive record's job.startedAt). */
#pragma once

#include "bro-id.h"
#include "bro-instant.h"
#include "bro-job-plan.h"
#include "bro-job-request.h"
#include "bro-step-output.h"

G_BEGIN_DECLS

typedef struct {
  BroId id;
  BroJobRequest *request;
  BroJobPlan *plan;   /* nullable */
  GPtrArray *outputs; /* BroStepOutput * */
  gboolean has_started_at;
  BroInstant started_at;
} BroJobSnapshot;

/* Takes ownership of @request and @plan. No outputs, no started_at. */
BroJobSnapshot *bro_job_snapshot_new (const BroId *id, BroJobRequest *request, BroJobPlan *plan);
void bro_job_snapshot_free (BroJobSnapshot *snapshot);

/* The data of the last output of that kind of step (borrowed); NULL when no such step ran. */
BroJsonValue *bro_job_snapshot_output (const BroJobSnapshot *job, BroStepKind kind);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobSnapshot, bro_job_snapshot_free)

G_END_DECLS
