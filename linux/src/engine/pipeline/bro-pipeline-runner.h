/* bro-pipeline-runner.h: BroPipelineRunner: the only code that moves a job between states (plan §12.3), on the
 * executor. Today's JobRunner.run()/execute() becomes JobPlanner (which steps), the step types (what each does),
 * JobOutcome.decide (the result) and this loop (the order, the resources, and the durable record of every step).
 * In C the loop is a chain of callbacks: each await of the other two is a worker or a scheduler answer arriving on the
 * executor. shared/fixtures/engine/pipeline-runner.cases.json. */
#pragma once

#include "bro-coalescer.h"
#include "bro-decided.h"
#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-job-dto.h"
#include "bro-scheduler.h"
#include "bro-step-environment.h"
#include "bro-step-registry.h"
#include "bro-store.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PIPELINE_RUNNER (bro_pipeline_runner_get_type ())
G_DECLARE_FINAL_TYPE (BroPipelineRunner, bro_pipeline_runner, BRO, PIPELINE_RUNNER, GObject)

/* How a run ended, on the executor: @decided, or @error (job.notFound) when there was no such job. Both borrowed. */
typedef void (*BroRunDone) (const BroDecided *decided, const BroBroError *error, gpointer data);

/* No job is live any more (after bro_pipeline_runner_shutdown), on the executor. */
typedef void (*BroRunnerIdle) (gpointer data);

/* Call on the executor. Keeps references; copies @environment. */
BroPipelineRunner *bro_pipeline_runner_new (BroEngineExecutor *executor, BroEventBus *bus, BroScheduler *scheduler, BroStore *store,
                                            BroStepRegistry *registry, const BroStepEnvironment *environment, BroEngineClock *clock);

/* Runs the job to its end, or until it is cancelled, then calls @done; @destroy frees @data afterwards. The job is live
 * (cancel reaches it) as soon as this is called, before its record is loaded. After a shutdown the run ends wherever it
 * is, storing nothing more, with server.stopping. */
void bro_pipeline_runner_run (BroPipelineRunner *self, const BroId *job_id, BroRunDone done, gpointer data, GDestroyNotify destroy);

/* At engine stop (decision 170): every live job's waiting request and question are dropped and its running step's token
 * cancelled, and from now on the runner stores nothing more for any job (no step row, plan, outcome or jobFinished), so
 * what is stored is what a killed engine leaves and the next start's recovery takes each job up. Calls @done once no
 * job is live (at once when none is); @destroy frees @data afterwards. */
void bro_pipeline_runner_shutdown (BroPipelineRunner *self, BroRunnerIdle done, gpointer data, GDestroyNotify destroy);

/* Cancels a running job: a waiting request or a decision ends at once, a running step sees its token. */
void bro_pipeline_runner_cancel (BroPipelineRunner *self, const BroId *job_id);

/* The job as it runs (progress, blockedBy, decision; free with bro_job_dto_free); NULL when it isn't running here or its
 * record isn't loaded yet. */
BroJobDto *bro_pipeline_runner_job (BroPipelineRunner *self, const BroId *job_id);

/* The running job's log (BroLogLineDto; yours): what the file held when it started, then this run's lines, written or
 * not; NULL when it isn't running here or hasn't read its log yet. */
GPtrArray *bro_pipeline_runner_log (BroPipelineRunner *self, const BroId *job_id);

/* Answers the job's question. FALSE and @error: job.notAwaitingDecision when it has none; server.badRequest for an
 * answer the question doesn't offer. */
gboolean bro_pipeline_runner_decide (BroPipelineRunner *self, const BroId *job_id, const char *answer, BroBroError **error);

G_END_DECLS
