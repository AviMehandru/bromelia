/* bro-job-planner.h: BroJobPlanner: the steps a job starts with (plan §15.2, §20.1); the Plan step appends the rest.
 * shared/fixtures/engine/job-planner.cases.json. */
#pragma once

#include "bro-job-request.h"
#include "bro-queue.h"
#include "bro-step-kind.h"
#include "bro-step-record.h"

G_BEGIN_DECLS

/* A video disc: awaitMedia when the rip is automatic and from a drive, probe, reconcile when a session made the
 * choices, identify, plan. Kinds whose steps aren't built yet get none. BroStepKind; free with g_array_unref. */
GArray *bro_job_planner_initial (const BroJobRequest *request);

/* The queue a job of the kind starts in (plan §21): a job that reads a disc, acquisition; checks and copies of the
 * archive, maintenance; commands and transcodes, processing. */
BroQueue bro_job_planner_queue (BroJobKind kind);

/* The steps a run doesn't do again (plan §12.3): of @steps (BroStepRecord), the leading ones, by seq, that succeeded or
 * were skipped, or failed after the commit (bro_job_outcome_after_commit: the runner went on after them). Copies, in a
 * new array that frees them. */
GPtrArray *bro_job_planner_done (GPtrArray *steps);

G_END_DECLS
