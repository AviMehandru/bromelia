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

/* Call on the executor. Keeps references; copies @environment. */
BroPipelineRunner *bro_pipeline_runner_new (BroEngineExecutor *executor, BroEventBus *bus, BroScheduler *scheduler, BroStore *store,
                                            BroStepRegistry *registry, const BroStepEnvironment *environment, BroEngineClock *clock);

/* Runs the job to its end, or until it is cancelled, then calls @done; @destroy frees @data afterwards. */
void bro_pipeline_runner_run (BroPipelineRunner *self, const BroId *job_id, BroRunDone done, gpointer data, GDestroyNotify destroy);

/* Cancels a running job: a waiting request or a decision ends at once, a running step sees its token. */
void bro_pipeline_runner_cancel (BroPipelineRunner *self, const BroId *job_id);

/* Answers the job's question. FALSE and @error: job.notAwaitingDecision when it has none; server.badRequest for an
 * answer the question doesn't offer. */
gboolean bro_pipeline_runner_decide (BroPipelineRunner *self, const BroId *job_id, const char *answer, BroBroError **error);

G_END_DECLS
