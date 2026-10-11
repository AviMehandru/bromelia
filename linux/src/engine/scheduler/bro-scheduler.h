/* bro-scheduler.h: BroScheduler: the three queues, the resources steps ask for, and space reservations (plan §21),
 * on the executor. Replaces today's queue pump (one running job per drive lane, maxConcurrentJobs) and the
 * background queue (backgroundJobs at once). shared/fixtures/engine/scheduler.cases.json.
 *
 * A drive has room for one job, and a job keeps a drive it was given until release_drives or finish; a step's other
 * resources go back on release. Acquisition slots: limits.acquisitions (0 = no cap; a job put first by start_now
 * isn't held back by it). Library writes: unlimited, blocked while the library is offline. Cpu: limits.cpu. Io per
 * volume: limits.io_per_volume. Every request of a job in the processing queue also takes a processing slot
 * (limits.processing) while its step runs. A maintenance job waits while any acquisition slot is held. Requests are
 * considered by queue, then the job's place in it, then request order; a request that has to wait keeps later ones
 * from overtaking it only on the resources it waits for. */
#pragma once

#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-lease-snapshot.h"
#include "bro-lease.h"
#include "bro-queue-change.h"
#include "bro-queue-orders.h"
#include "bro-queue.h"
#include "bro-resource-set.h"
#include "bro-scheduler-limits.h"
#include "bro-wait.h"
#include "bro-cancellation-token.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SCHEDULER (bro_scheduler_get_type ())
G_DECLARE_FINAL_TYPE (BroScheduler, bro_scheduler, BRO, SCHEDULER, GObject)

/* A change while a request waits; @wait is borrowed. */
typedef void (*BroWaitFunc) (const BroWait *wait, gpointer data);

/* The request's end, on the executor: @lease (a new reference) when granted, else @error (borrowed). */
typedef void (*BroAcquireDone) (BroLease *lease, const BroBroError *error, gpointer data);

BroScheduler *bro_scheduler_new (BroEngineExecutor *executor, BroEventBus *bus, const BroSchedulerLimits *limits);

/* Adds a job at the end of a queue. */
void bro_scheduler_submit (BroScheduler *self, const BroId *job_id, BroQueue queue);

/* Moves a job to the end of another queue (after Release: processing). */
void bro_scheduler_move_to (BroScheduler *self, const BroId *job_id, BroQueue queue);

/* Cancels the job's waiting requests, gives back everything it holds and its space, takes it out of its queue. */
void bro_scheduler_finish (BroScheduler *self, const BroId *job_id);

/* Moves a job within its queue, stopping at the ends. FALSE and @error (job.notQueued) when it isn't in one. */
gboolean bro_scheduler_move (BroScheduler *self, const BroId *job_id, int by, BroBroError **error);

/* Puts a job first in its queue; from now on the acquisitions cap doesn't hold it back. */
gboolean bro_scheduler_start_now (BroScheduler *self, const BroId *job_id, BroBroError **error);

/* Asks for @resources. @done runs once, posted to the executor (never inside this call): with the lease when
 * everything has room, or with job.cancelled when @cancel fires (or the job finishes) first. @on_wait hears each
 * change while the request waits. @destroy frees @data afterwards (also when the executor stopped before @done
 * could run). FALSE and @error (job.notQueued, @done never runs, @data destroyed) for a job in no queue. */
gboolean bro_scheduler_acquire (BroScheduler *self, const BroId *job_id, const BroResourceSet *resources, BroWaitFunc on_wait,
                                BroCancellationToken *cancel, BroAcquireDone done, gpointer data, GDestroyNotify destroy,
                                BroBroError **error);

/* Gives back a lease, but not its drives. Twice does nothing. */
void bro_scheduler_release (BroScheduler *self, BroLease *lease);

/* Gives back the job's drives (the Release step). */
void bro_scheduler_release_drives (BroScheduler *self, const BroId *job_id);

/* Cancels every waiting request of the jobs in @queue. */
void bro_scheduler_cancel_all (BroScheduler *self, BroQueue queue);

/* Keeps @need bytes on @volume for the job, replacing what it kept there. FALSE and space.insufficient {path, need,
 * free} when @free minus what other jobs keep there is less. */
gboolean bro_scheduler_reserve (BroScheduler *self, const BroId *job_id, const char *volume, const char *path, gint64 need,
                                gint64 free, BroBroError **error);

void bro_scheduler_unreserve (BroScheduler *self, const BroId *job_id);

/* @reason (library.offline; copied) blocks writes to the library; NULL lets them through again. */
void bro_scheduler_set_library_offline (BroScheduler *self, const char *library_id, const BroBroError *reason);

void bro_scheduler_update_limits (BroScheduler *self, const BroSchedulerLimits *limits);

/* Free with bro_queue_orders_free. */
BroQueueOrders *bro_scheduler_queues (BroScheduler *self);

/* Free with bro_lease_snapshot_free. */
BroLeaseSnapshot *bro_scheduler_leases (BroScheduler *self);

G_END_DECLS
