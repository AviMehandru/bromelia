/* bro-job-outcome.h: BroJobOutcome, how a job ended, from its steps' results (plan §20.3): one pure function
 * instead of today's chain of flags in three run() methods. */
#pragma once

#include "bro-decided.h"
#include "bro-outcome-policy.h"
#include "bro-step-result.h"

G_BEGIN_DECLS

/* @step_results: BroStepResult *. In order: cancelled (job.cancelledByUser); interrupted (recovery.interrupted);
 * skipped (the skip reason); a failed step that isn't after the commit fails the job with its error, or with the
 * MakeMKV problem that explains it (the error as its cause); read errors give succeededWithReadErrors
 * (rip.readErrors); a failed post-commit step that affects the outcome gives succeededStepsFailed; else
 * succeeded. */
BroDecided *bro_job_outcome_decide (GPtrArray *step_results, gboolean cancelled, const BroOutcomePolicy *policy);

G_END_DECLS
