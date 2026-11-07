/* bro-recovery-service.h: BroRecoveryService: runs once at startup, before the libraries start (plan §11.1, §15.1,
 * §20.4, §15.8). It sorts out the jobs the last engine left unfinished by where each stopped and what its step can do
 * after a stop (its Resumability), rolls open commit intents forward, sets aside staging folders no job will continue,
 * and removes the temporary files a stopped write leaves. Library work runs on a worker per library and may take at
 * most 30 s of the startup: a share that doesn't answer can't keep the engine from starting, and its work goes on.
 * shared/fixtures/engine/recovery-service.cases.json. */
#pragma once

#include "bro-config-source.h"
#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-file-system.h"
#include "bro-library-service.h"
#include "bro-message-renderer.h"
#include "bro-problem-service.h"
#include "bro-settings-isolation.h"
#include "bro-step-registry.h"
#include "bro-store.h"

G_BEGIN_DECLS

#define BRO_TYPE_RECOVERY_SERVICE (bro_recovery_service_get_type ())
G_DECLARE_FINAL_TYPE (BroRecoveryService, bro_recovery_service, BRO, RECOVERY_SERVICE, GObject)

/* Runs on the executor: @summary is recovery.summary{count} when the last engine left jobs unfinished (yours to free),
 * NULL otherwise or on failure (@error set). */
typedef void (*BroRecoveryDone) (BroBroMessage *summary, const BroBroError *error, gpointer data);

/* Keeps references to what it is given. @data_directory: the engine's data folder; a job's folder is
 * <data>/jobs/<id>. */
BroRecoveryService *bro_recovery_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroStore *store,
                                              BroStepRegistry *registry, BroLibraryService *library, BroConfigSource *config,
                                              BroEngineClock *clock, BroFileSystem *fs, BroSettingsIsolation *isolation,
                                              BroMessageRenderer *renderer, const char *data_directory);

/* On the executor. @done runs once the jobs are sorted out and the libraries tidied, or 30 s after it started, whichever
 * comes first; @destroy frees @data afterwards. */
void bro_recovery_service_recover (BroRecoveryService *self, BroRecoveryDone done, gpointer data, GDestroyNotify destroy);

G_END_DECLS
