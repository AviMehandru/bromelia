/* bro-job-service.h: BroJobService: the jobs (plan §11.1, §13.1). It creates them, runs them through the PipelineRunner
 * and keeps the in-memory view of the ones that haven't finished; the Scheduler decides when each step runs. On the
 * executor; the store and the log files are read and written on workers. A created job is in its queue at once, in the
 * order the jobs were created, and in the store before jobChanged (queued) announces it; then it runs. start() runs the
 * jobs the last engine left unfinished (after recover()), in their queues' order. Each job's place in its queue is
 * stored when it changes (a transaction that changes only the position: the runner owns the rest of the record). The
 * continue offer comes with AutomationService (owner, 2026-10-17/18). shared/fixtures/engine/job-service.cases.json. */
#pragma once

#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-file-system.h"
#include "bro-id-generator.h"
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

/* Runs a finished job again (plan §13.1's retry(id, fromStep?); owner 2026-10-17/18).
 * - Without a step (@from_step NULL): one that failed, was cancelled or was skipped, as a new job with the same request,
 *   started by hand (not automatic, no media generation); an interrupted one is continued: the same job from its first
 *   step.
 * - From a step: one that failed, was cancelled or was interrupted, the same job, from that step (job.unknownStep {step}
 *   when its plan has none; server.badRequest when a step before it isn't done). A step up to Release starts it from its
 *   first step instead: the disc is read again (Acquire then keeps what an earlier attempt finished of the same disc).
 * The same job's steps from there are stored pending (their checkpoints kept), its plan cut back after Plan when Plan runs
 * again, and its record queued again (no outcome, by hand, no media generation; acquisition up to Release, else
 * processing) with job.retried {step} in its log, then it runs like a new one. job.notFound; job.notRetryable for any
 * other job (read errors and failed post steps get retries of their own). While the job has an open commit intent, only a
 * retry from Commit is taken (job.commitOpen). A job that was kept apart is retried from its keepApart step only (that
 * step again, in the processing queue), or as a new job; any other step is job.notRetryable. */
void bro_job_service_retry (BroJobService *self, const BroId *job_id, const BroStepKind *from_step, BroJobDtoDone done, gpointer data,
                            GDestroyNotify destroy);

/* Doesn't continue an interrupted job (owner, 2026-10-03 and 2026-10-25; plan §20.4): the same job runs again with a
 * keepApart step in place of its interrupted step and what follows (KeepApartStep seals what staging holds and quarantines
 * it as INCOMPLETE; the job then ends cancelled). The step's row is stored pending (output, error and checkpoint cleared),
 * the plan cut back to the steps before it plus keepApart, and the record queued again in the processing queue (no
 * outcome; the request as it was); jobChanged, then it runs. job.notFound; job.notInterrupted for a job that isn't
 * finished and interrupted, that this service runs, or whose Plan step didn't succeed. */
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

G_END_DECLS
