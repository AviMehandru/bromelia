/* bro-test-composition-root.h: TestCompositionRoot: builds the engine the real composition root builds (decision 174):
 * the same services, bro_step_registry_standard and BroLocalEngine, over the test kit's fakes. A manual clock; files in
 * memory under TEST_ROOT (the data folder is TEST_ROOT_DATA_DIR); SQLite on a file in a temporary folder of the real
 * disk, so restart reopens the same database; scripted processes under the real MakemkvTool and MkvToolNix; the fake
 * drives; an in-memory keystore pair; counting ids; a recording engine log; English messages and app version "test".
 * Build with test_composition_root_new, then start. A scenario runner passes its own launcher for the tools, and kills the
 * engine at a crash point (test_composition_root_kill, then test_composition_root_restart). Test code, not shipped. TestWaiter is how the tests wait for an
 * engine's callbacks. */
#pragma once

#include "bro-local-engine.h"
#include "bro-sqlite-store.h"
#include "bro-test-kit.h"

G_BEGIN_DECLS

#define TEST_ROOT "/root"
#define TEST_ROOT_DATA_DIR TEST_ROOT "/data"
#define TEST_ROOT_CONFIG_PATH TEST_ROOT_DATA_DIR "/config.json"
#define TEST_ROOT_MAKEMKVCON "/tools/makemkvcon"
#define TEST_ROOT_MKVMERGE "/tools/mkvmerge"

/* ---- waiting for a callback ---------------------------------------------------------------------------------------- */

/* One answer a test waits for: a result, an error, or both missing until it comes. Reference counted, so a wait that
 * gave up (a call that never ended) leaves nothing dangling. */
typedef struct {
  gatomicrefcount refs;
  GMutex lock;
  GCond cond;
  gboolean finished;
  BroJsonValue *result; /* nullable */
  BroBroError *error;   /* nullable */
  gpointer object;      /* a GObject the answer gave (a feed), a reference; nullable */
} TestWaiter;

TestWaiter *test_waiter_new (void);
TestWaiter *test_waiter_ref (TestWaiter *waiter);
void test_waiter_unref (gpointer waiter);
/* The answer: takes @result and @object (each nullable), copies @error (nullable). Any thread; the first one counts. */
void test_waiter_finish (TestWaiter *waiter, BroJsonValue *result, gpointer object, const BroBroError *error);
/* TRUE once finished, FALSE when @seconds of real time passed first. */
gboolean test_waiter_wait (TestWaiter *waiter, double seconds);
/* A BroEngineDone that finishes the waiter given as data (with test_waiter_ref/unref as data and destroy). */
void test_waiter_done (const BroBroError *error, gpointer waiter);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (TestWaiter, test_waiter_unref)

/* ---- the root ------------------------------------------------------------------------------------------------------ */

typedef struct {
  char *folder; /* the temporary folder on disk that holds bromelia.db */
  TestManualClock *clock;
  TestMemoryFs *fs;
  TestScriptedLauncher *launcher;
  BroProcessLauncher *tool_launcher; /* what MakemkvTool and MkvToolNix start with: the constructor's, else launcher */
  TestRecordingIsolation *isolation;
  TestFixedLocator *locator;
  TestFakeDrives *drives;
  TestMemoryKeystore *keystore;
  TestMemoryKeystore *secret_files;
  TestIds *ids;
  TestRecordingEngineLog *log;
  GHashTable *environment; /* the BROMELIA_SECRET_* variables the root hands ConfigService */
  int cores;
  guint stream_capacity;
  /* Called when an engine's release runs (after its executor stopped), before the store closes. */
  void (*on_release) (gpointer data);
  gpointer on_release_data;
  /* The crash hook of LibraryService (commit.*) and PipelineRunner (beforeStep:/afterStep:<kind>), settable any time: FALSE
   * (the error test_composition_root_kill gives) stops the engine there as a killed one would. */
  BroCrashPointFunc crash;
  gpointer crash_data;
  double last_stop;   /* how long the last stop took, in real seconds */
  gboolean killed;    /* killed at killed_at, not finished yet (restart finishes it) */
  char *killed_at;
  gint64 killed_seq;  /* the bus's last seq when it was killed: what it publishes after that never happened */
  /* The graph built last (a harness peeks at the store; the engine owns it). */
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroLibraryService *libraries;
  BroJobService *jobs;
  BroSqliteStore *store;
  BroLocalEngine *engine;
  gboolean stopped;
} TestCompositionRoot;

/* A stop that takes this long hit LocalEngine's cap (G8 N1: a restart after it fails the case). */
#define TEST_ROOT_STOP_CAP 30.0
/* How long a killed engine has to go quiet before its executor is stopped. */
#define TEST_ROOT_KILL_CAP 10.0

/* @makemkvcon_version: what the locator reports as makemkvcon's version (nullable). @environment: copied (nullable:
 * none). @cores: the processor count in BroEngineFacts. @stream_capacity: the bus's (a small one makes a feed's
 * reader fall behind). @launcher: what the tools start their processes with (a scenario's scripted tools; a reference;
 * NULL: the root's launcher). */
TestCompositionRoot *test_composition_root_new (gboolean makemkvcon, const char *makemkvcon_version, gboolean mkvmerge,
                                                GHashTable *environment, int cores, guint stream_capacity, BroProcessLauncher *launcher);
/* Stops the engine when it runs (waiting at most 40 s), closes the store and deletes the folder. */
void test_composition_root_free (TestCompositionRoot *root);

/* bro_local_engine_start; the engine counts as running from now on, even when start fails. Wait on the waiter. */
TestWaiter *test_composition_root_start (TestCompositionRoot *root);
/* The engine's stop (once: a waiter already finished when it is stopped). */
TestWaiter *test_composition_root_stop (TestCompositionRoot *root);
/* The stop, waited for at most @seconds and timed in real time (last_stop). FALSE when it never ended. */
gboolean test_composition_root_stop_timed (TestCompositionRoot *root, double seconds);
/* Stops the engine (when running), then builds a new one on the same database and files and starts it. After a kill it
 * finishes the kill instead of stopping. FALSE (and @error, or nothing when it never ended within @seconds each) when the
 * stop or the start failed; *@why (nullable) says so in words: a stop that hit the 30 s cap (G8 N1), a killed engine that
 * didn't go quiet within 10 s. */
gboolean test_composition_root_restart (TestCompositionRoot *root, double seconds, BroBroError **error, char **why);
/* Kills the engine at a crash point (decision 240), called at the point (on the executor or a worker): the files freeze
 * (no write lands from now on), JobService's stop is posted ahead of whatever the point unwinds into (so the runner stores
 * nothing more) and the bus's last seq is noted. Returns the error the point must fail with. The store stays open until
 * the old executor is quiet (restart): closing it in the middle of a call isn't safe. */
BroBroError *test_composition_root_kill (TestCompositionRoot *root, const char *point);
/* The library folder's volume goes away (its files are kept aside), and LibraryService checks at once (owner answer 10:
 * no OS event reports a library's volume); mount brings it back. FALSE when the check never ended within @seconds. */
gboolean test_composition_root_unmount (TestCompositionRoot *root, const char *path, double seconds);
gboolean test_composition_root_mount (TestCompositionRoot *root, const char *path, double seconds);

G_END_DECLS
