/* bro-job-service.h: BroJobService: the jobs (plan §11.1, §13.1). It creates them, runs them through the PipelineRunner
 * and keeps the in-memory view of the ones that haven't finished; the Scheduler decides when each step runs. On the
 * executor; the store and the log files are read and written on workers. A created job is in its queue at once, in the
 * order the jobs were created, and in the store before jobChanged (queued) announces it; then it runs. start() runs the
 * jobs the last engine left unfinished (after recover()), in their queues' order. Each job's place in its queue is
 * stored when it changes (a transaction that changes only the position: the runner owns the rest of the record). The
 * continue offer comes with AutomationService. stop() leaves the running jobs as a killed engine would: the next
 * start's recovery takes them up. shared/fixtures/engine/job-service.cases.json. */
#pragma once

#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-file-system.h"
#include "bro-id-generator.h"
#include "bro-job-detail-dto.h"
#include "bro-job-dto.h"
#include "bro-job-page-dto.h"
#include "bro-job-query-dto.h"
#include "bro-pipeline-runner.h"
#include "bro-scheduler.h"
#include "bro-store.h"

G_BEGIN_DECLS

#define BRO_TYPE_JOB_SERVICE (bro_job_service_get_type ())
G_DECLARE_FINAL_TYPE (BroJobService, bro_job_service, BRO, JOB_SERVICE, GObject)

/* How a command ended, on the executor: @error (borrowed) on failure, else NULL. */
typedef void (*BroJobServiceDone) (const BroBroError *error, gpointer data);

/* A command that gives a job: @job (borrowed), or @error. */
typedef void (*BroJobDtoDone) (const BroJobDto *job, const BroBroError *error, gpointer data);

/* query: @page (borrowed), or @error. */
typedef void (*BroJobPageDone) (const BroJobPageDto *page, const BroBroError *error, gpointer data);

/* log: @lines (BroLogLineDto, borrowed), or @error. */
typedef void (*BroLogLinesDone) (GPtrArray *lines, const BroBroError *error, gpointer data);
/* job: @detail (borrowed), or @error. */
typedef void (*BroJobDetailDone) (const BroJobDetailDto *detail, const BroBroError *error, gpointer data);

/* Keeps references to what it is given. @data_directory: the engine's data folder; a job's log is
 * <data>/jobs/<id>/log.jsonl. */
BroJobService *bro_job_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroStore *store, BroScheduler *scheduler,
                                    BroPipelineRunner *runner, BroEngineClock *clock, BroIdGenerator *ids, BroFileSystem *fs,
                                    const char *data_directory);

/* Every command runs on the executor and calls @done there once (also on failure); @destroy frees @data afterwards. */

/* Runs the jobs the store has unfinished (what RecoveryService left queued), in their queues' order. Once, after
 * recover(). */
void bro_job_service_start (BroJobService *self, BroJobServiceDone done, gpointer data, GDestroyNotify destroy);

/* A new job for @request (copied): queued, stored, announced (jobChanged with its position), then run. */
void bro_job_service_create (BroJobService *self, const BroJobRequest *request, BroJobDtoDone done, gpointer data, GDestroyNotify destroy);

/* A waiting or running job ends cancelled (job.cancelledByUser); one nothing runs is finished as cancelled at once; a
 * finished one stays as it is. job.notFound. */
void bro_job_service_cancel (BroJobService *self, const BroId *job_id, BroJobServiceDone done, gpointer data, GDestroyNotify destroy);

/* Runs a finished job again (plan §13.1's retry(id, fromStep?)). Without a step (@from_step NULL), a job that failed,
 * was cancelled or was skipped runs as a new job with the same request, started by hand; an interrupted one is
 * continued from its first step. From a step, a job that failed, was cancelled or was interrupted runs as the same job
 * from there; a step up to Release starts it from its first step instead, as the disc is read again (Acquire then keeps
 * what an earlier attempt finished of the same disc). The same job's steps from there are stored pending, their
 * checkpoints kept, and its plan is cut back after Plan when Plan runs again.
 *
 * While the job has an open commit intent its files are partly in the library, so only a retry from Commit, which
 * finishes it, is taken (job.commitOpen): a step before Commit would change staging under the intent, and a new job
 * would archive the disc again beside it. Once the unit is in a library, a retry from a step before Commit, or
 * continuing it, is job.alreadyCommitted: the same job would rip the disc again as the same unit. A job kept apart is
 * retried from its keepApart step only, or as a new job. */
void bro_job_service_retry (BroJobService *self, const BroId *job_id, const BroStepKind *from_step, BroJobDtoDone done, gpointer data,
                            GDestroyNotify destroy);

/* Doesn't continue an interrupted, failed or cancelled job (plan §20.4): the same job runs again with a keepApart step
 * in place of the step it stopped at and what follows. KeepApartStep seals what staging holds and quarantines it as
 * INCOMPLETE; the job then ends cancelled. The step's row is stored pending with the checkpoint
 * bro_keep_apart_step_kept (the job's outcome and error) for its note, the plan is cut back to the steps before it plus
 * keepApart, and the record is queued again in the processing queue.
 *
 * job.notInterrupted for a job that didn't end that way, was kept apart already (retry its keepApart step instead), is
 * running here, or whose Plan step didn't succeed. job.processAbandoned for a job that logged process.abandoned in this
 * engine run: the tool may still be writing to its staging (after a restart it is kept apart as any other).
 * job.commitOpen while it has an open commit intent; job.alreadyCommitted once its unit is in a library. */
void bro_job_service_keep_apart (BroJobService *self, const BroId *job_id, BroJobDtoDone done, gpointer data, GDestroyNotify destroy);

/* Answers the job's question @decision_id. job.notFound; job.notAwaitingDecision when the job isn't waiting for that
 * question; server.badRequest for an answer it doesn't offer. */
void bro_job_service_decide (BroJobService *self, const BroId *job_id, const char *decision_id, const char *answer, BroJobServiceDone done,
                             gpointer data, GDestroyNotify destroy);

/* Moves a job that waits to start a step @by places in its queue (negative: earlier), stopping at the ends.
 * job.notFound; job.notQueued for a job that runs a step, waits for an answer or has finished. */
void bro_job_service_move (BroJobService *self, const BroId *job_id, int by, BroJobServiceDone done, gpointer data, GDestroyNotify destroy);

/* Puts a job that waits to start a step first in its queue; the acquisitions cap no longer holds it back. job.notFound;
 * job.notQueued as for move. */
void bro_job_service_start_now (BroJobService *self, const BroId *job_id, BroJobServiceDone done, gpointer data, GDestroyNotify destroy);

/* A page of jobs, newest first: running jobs as they run (progress, blockedBy, decision), waiting ones with their place
 * in the queue. The states are the stored ones (queued, running, finished). server.badRequest for a cursor that isn't
 * one or a limit outside 1-500. */
void bro_job_service_query (BroJobService *self, const BroJobQueryDto *query, BroJobPageDone done, gpointer data, GDestroyNotify destroy);

/* The job's log lines after seq @after (0: all), including a running step's lines not yet written. job.notFound. */
void bro_job_service_log (BroJobService *self, const BroId *job_id, gint64 after, BroLogLinesDone done, gpointer data, GDestroyNotify destroy);
/* The job with its steps, files and commands (dtos.json JobDetail). job: as query gives it. steps: one per step of its
 * plan (the stored plan, else the planner's steps), each with the stored row of that seq when it is of that kind, else
 * pending; rows past the plan's end (a keepApart cut the plan back) are left out. files: the Seal row's files, else the
 * Acquire row's produced files (names in staging), else none. commands: none yet (nothing logs process.command).
 * logBytes: the size of its log.jsonl. job.notFound. */
void bro_job_service_job (BroJobService *self, const BroId *job_id, BroJobDetailDone done, gpointer data, GDestroyNotify destroy);
/* At engine stop: the runner shuts every live job down without storing anything more for it
 * (bro_pipeline_runner_shutdown) and no queue place is stored any more; @done (no error) once no job is live. The
 * commands answer server.stopping from now on. */
void bro_job_service_stop (BroJobService *self, BroJobServiceDone done, gpointer data, GDestroyNotify destroy);

G_END_DECLS
