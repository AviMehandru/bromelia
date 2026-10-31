/* bro-local-engine.h: BroLocalEngine: implements BroEngineAPI over the services (plan §13.1, §15.1). Callable from any
 * thread but the executor's: each call hops onto the BroEngineExecutor, and its @done runs there (a call the executor no
 * longer takes answers server.stopping on the caller's thread). The composition root builds it, then the host calls
 * bro_local_engine_start and calls nothing else before that completes. After stop every call answers server.stopping.
 * shared/fixtures/engine/local-engine.cases.json. */
#pragma once

#include "bro-config-service.h"
#include "bro-drive-service.h"
#include "bro-engine-api.h"
#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-engine-facts.h"
#include "bro-engine-log.h"
#include "bro-event-bus.h"
#include "bro-job-request.h"
#include "bro-job-service.h"
#include "bro-library-service.h"
#include "bro-problem-service.h"
#include "bro-recovery-service.h"
#include "bro-scheduler.h"
#include "bro-tool-locator.h"

G_BEGIN_DECLS

#define BRO_TYPE_LOCAL_ENGINE (bro_local_engine_get_type ())
G_DECLARE_FINAL_TYPE (BroLocalEngine, bro_local_engine, BRO, LOCAL_ENGINE, GObject)

/* Called once stop has stopped the executor: the root closes the store there. */
typedef void (*BroEngineRelease) (gpointer data);

/* Keeps references to what it is given and copies @facts. @release (@release_data, which the engine never frees) runs
 * once, at the end of stop. */
BroLocalEngine *bro_local_engine_new (BroEngineExecutor *executor, BroEventBus *bus, BroEngineClock *clock, BroProblemService *problems,
                                      BroConfigService *config, BroScheduler *scheduler, BroLibraryService *libraries,
                                      BroDriveService *drives, BroJobService *jobs, BroRecoveryService *recovery, BroToolLocator *locator,
                                      BroEngineLog *log, const BroEngineFacts *facts, BroEngineRelease release, gpointer release_data);

/* The startup order (plan §15.1, decision 169): the configuration (a failed load fails start, and nothing else has
 * started), recovery (recovery.summary into the engine log), the configuration's limits to the Scheduler (and on every
 * configChanged), the libraries, the drives, then the jobs recovery left queued. A start that failed leaves the engine
 * as it is: the host stops it. */
void bro_local_engine_start (BroLocalEngine *self, BroEngineDone done, gpointer data, GDestroyNotify destroy);

/* A phase 3 door outside EngineAPI (G1 owner question 5): a job for @request (an ISO or a folder needs SessionService
 * otherwise), for the scenario runner and the real-ISO test. Retired when openSession/ripSession come. */
void bro_local_engine_start_job (BroLocalEngine *self, const BroJobRequest *request, BroEngineJobDone done, gpointer data,
                                 GDestroyNotify destroy);

G_END_DECLS
