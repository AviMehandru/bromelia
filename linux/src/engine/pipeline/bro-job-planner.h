/* bro-job-planner.h: BroJobPlanner: the steps a job starts with (plan §15.2, §20.1); the Plan step appends the rest.
 * shared/fixtures/engine/job-planner.cases.json. */
#pragma once

#include "bro-job-request.h"
#include "bro-step-kind.h"

G_BEGIN_DECLS

/* A video disc: awaitMedia when the rip is automatic and from a drive, probe, reconcile when a session made the
 * choices, identify, plan. Kinds whose steps aren't built yet get none. BroStepKind; free with g_array_unref. */
GArray *bro_job_planner_initial (const BroJobRequest *request);

G_END_DECLS
