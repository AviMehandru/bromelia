/* bro-library-service.h: BroLibraryService: the libraries of the configuration (plan §11.1, §22), used on the
 * executor. A library is online while its folder holds the .bromelia-library marker it was attached with (I2): the
 * markers are checked at start, every 30 s and when a volume is mounted or unmounted, each on its own worker; an
 * offline library is blocked in the scheduler and raised as a problem. A check that hasn't come back by the next tick (a
 * share that froze, review L3-9) counts as offline and isn't started again until it comes back. As the BroLibraryPort
 * of the steps it gives staging folders and runs the commit protocol (§22.3 steps 4–6) and its roll forward (§15.8) on
 * the caller's worker, one at a time per unit: a commit waits while a roll forward of its unit runs (recovery's may
 * outlast its 30 s at startup on a slow share). State is touched only on the executor; the steps read a snapshot.
 * shared/fixtures/engine/library-service.cases.json. */
#pragma once

#include "bro-commit-intent.h"
#include "bro-commit-result.h"
#include "bro-config-source.h"
#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-file-system.h"
#include "bro-library-dto.h"
#include "bro-library-port.h"
#include "bro-library-repository.h"
#include "bro-problem-service.h"
#include "bro-scheduler.h"
#include "bro-unit-repository.h"

G_BEGIN_DECLS

#define BRO_TYPE_LIBRARY_SERVICE (bro_library_service_get_type ())
G_DECLARE_FINAL_TYPE (BroLibraryService, bro_library_service, BRO, LIBRARY_SERVICE, GObject)

/* A crash point of the commit (commit.afterIntent, commit.afterMove:<n>, commit.beforeDone): FALSE (with @error set)
 * stops the commit there, as a killed engine would. Only a test root passes one. */
typedef gboolean (*BroCrashPointFunc) (const char *point, gpointer data, BroBroError **error);

/* Runs on the executor, never inside the call that started the work: @error is NULL when it succeeded. */
typedef void (*BroLibraryDone) (const BroBroError *error, gpointer data);
/* The same with the library (yours to free; NULL on failure). */
typedef void (*BroLibraryDtoDone) (BroLibraryDto *library, const BroBroError *error, gpointer data);

/* @crash (@crash_data) may be NULL. */
BroLibraryService *bro_library_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroScheduler *scheduler,
                                            BroConfigSource *config, BroEngineClock *clock, BroFileSystem *fs, BroLibraryRepository *libraries,
                                            BroUnitRepository *units, BroCrashPointFunc crash, gpointer crash_data);

/* Reads when each library was last seen, checks every library, then every 30 s, and follows configChanged; @done runs
 * when the first round is applied. @destroy frees @data afterwards. */
void bro_library_service_start (BroLibraryService *self, BroLibraryDone done, gpointer data, GDestroyNotify destroy);

/* Stops the timer and the configChanged subscription. */
void bro_library_service_stop (BroLibraryService *self);

/* In configuration order (on the executor): BroLibraryDto *, the caller frees the array. */
GPtrArray *bro_library_service_libraries (BroLibraryService *self);

/* Checks every marker now, as when a volume is mounted or unmounted; while a round is still out, waits for it instead. */
void bro_library_service_check (BroLibraryService *self, BroLibraryDone done, gpointer data, GDestroyNotify destroy);

/* Writes the marker into an existing folder (never creates it), stores the library and brings it online:
 * library.pathMissing{path}, library.alreadyAttached{path, library}. @name "" names it after its folder. */
void bro_library_service_attach (BroLibraryService *self, const char *library_id, const char *path, const char *name, BroLibraryDtoDone done,
                                 gpointer data, GDestroyNotify destroy);

/* Finishes an interrupted commit (§15.8): once no other commit of the unit runs, the marker checked, then what is left
 * of the unit's open intent (read again: @intent may be stale) moved (a file already at its destination counts when its
 * SHA-256 is the item's), SHA256SUMS merged, the folders synced and the unit committed, its files read from the
 * destination's SHA256SUMS. On a worker, never the executor. NULL and @error set on failure; NULL with @error unset when
 * the intent is no longer open (the job's Commit finished it meanwhile). */
BroCommitResult *bro_library_service_roll_forward (BroLibraryService *self, const BroCommitIntent *intent, BroBroError **error);

/* Commits and roll forwards waiting for another one of their unit to finish (a test or scenario runner counts them as
 * parked when it settles). Any thread. */
int bro_library_service_waiting (BroLibraryService *self);

/* The folders in the library's .bromelia/staging (one per job, named by its id), in ordinal order (char *, the caller
 * frees the array), once the marker checks out (from the configuration itself, so it works before start). On a
 * worker, never the executor. NULL and @error set on failure. */
GPtrArray *bro_library_service_staging_folders (BroLibraryService *self, const char *library_id, BroBroError **error);

/* A staging folder no job will continue goes to <library>/_Quarantine/<ConflictNamer('<@name> [INCOMPLETE]')> with
 * @note as INCOMPLETE.txt (written first, so the folder arrives marked: I1), never reused or deleted (review L3-3,
 * P1-3); the folders are synced. The new path (yours to free); NULL when the folder held no file and was removed
 * instead (@error unset), or on failure (@error set). On a worker, never the executor. */
char *bro_library_service_set_aside (BroLibraryService *self, const char *library_id, const char *folder, const char *name, GBytes *note,
                                     BroBroError **error);

G_END_DECLS
