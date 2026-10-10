/* bro-engine-executor.h: BroEngineExecutor: the one thread that owns all service state (plan §8). It runs a
 * GMainLoop on a GMainContext of its own. Actions posted from any thread run there one at a time, in the order they
 * were posted, never inside post. Blocking work goes to worker (a GThreadPool), whose outcome comes back on the
 * executor. */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ENGINE_EXECUTOR (bro_engine_executor_get_type ())
G_DECLARE_FINAL_TYPE (BroEngineExecutor, bro_engine_executor, BRO, ENGINE_EXECUTOR, GObject)

typedef void (*BroExecutorAction) (gpointer data);

/* Runs on a worker thread: does the blocking work, keeping what it produced in @data. FALSE and @error set on
 * failure. */
typedef gboolean (*BroWorkerFunc) (gpointer data, BroBroError **error);

/* Runs on the executor afterwards: @error is the work's error (NULL when it succeeded), owned by the executor. */
typedef void (*BroWorkerDone) (gpointer data, const BroBroError *error);

/* Starts the executor's thread, named @name. */
BroEngineExecutor *bro_engine_executor_new (const char *name);

/* Runs @action (@data) on the executor after everything posted before it; @destroy frees @data after it ran. FALSE
 * once the executor is stopping: the action never runs and @data is destroyed at once. */
gboolean bro_engine_executor_post (BroEngineExecutor *self, BroExecutorAction action, gpointer data, GDestroyNotify destroy);

/* TRUE on the executor's thread. */
gboolean bro_engine_executor_is_current (BroEngineExecutor *self);

/* Runs @work on a worker thread, then @done on the executor; @destroy frees @data after both. Once the executor is
 * stopping, @done doesn't run (@data is still destroyed). */
void bro_engine_executor_worker (BroEngineExecutor *self, BroWorkerFunc work, BroWorkerDone done, gpointer data,
                                 GDestroyNotify destroy);

/* Waits until every action posted before it has run (at once when stopped). Not on the executor. */
void bro_engine_executor_barrier (BroEngineExecutor *self);

/* Lets the queued actions run, then ends the thread; later posts don't run. Waits for the end, unless called on the
 * executor (it then ends after the current action and what is queued). Workers still running aren't waited for. */
void bro_engine_executor_stop (BroEngineExecutor *self);

G_END_DECLS
