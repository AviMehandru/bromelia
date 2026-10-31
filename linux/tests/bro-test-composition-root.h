/* bro-test-composition-root.h: TestCompositionRoot: builds the engine the real composition root builds (decision 174):
 * the same services, bro_step_registry_standard and BroLocalEngine, over the test kit's fakes. A manual clock; files in
 * memory under TEST_ROOT (the data folder is TEST_ROOT_DATA_DIR); SQLite on a file in a temporary folder of the real
 * disk, so restart reopens the same database; scripted processes under the real MakemkvTool and MkvToolNix; the fake
 * drives; an in-memory keystore pair; counting ids; a recording engine log; English messages and app version "test".
 * Build with test_composition_root_new, then start. Test code, not shipped. TestWaiter is how the tests wait for an
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
  /* LibraryService's crash hook (H's crash points), settable any time: FALSE stops a commit as a killed engine would. */
  BroCrashPointFunc crash;
  gpointer crash_data;
  /* The graph built last (a harness peeks at the store; the engine owns it). */
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroLibraryService *libraries;
  BroSqliteStore *store;
  BroLocalEngine *engine;
  gboolean stopped;
} TestCompositionRoot;

/* @makemkvcon_version: what the locator reports as makemkvcon's version (nullable). @environment: copied (nullable:
 * none). @cores: the processor count in BroEngineFacts. @stream_capacity: the bus's (a small one makes a feed's
 * reader fall behind). */
TestCompositionRoot *test_composition_root_new (gboolean makemkvcon, const char *makemkvcon_version, gboolean mkvmerge,
                                                GHashTable *environment, int cores, guint stream_capacity);
/* Stops the engine when it runs (waiting at most 40 s), closes the store and deletes the folder. */
void test_composition_root_free (TestCompositionRoot *root);

/* bro_local_engine_start; the engine counts as running from now on, even when start fails. Wait on the waiter. */
TestWaiter *test_composition_root_start (TestCompositionRoot *root);
/* The engine's stop (once: a waiter already finished when it is stopped). */
TestWaiter *test_composition_root_stop (TestCompositionRoot *root);
/* Stops the engine (when running), then builds a new one on the same database and files and starts it. FALSE (and
 * @error, or nothing when it never ended within @seconds each) when the stop or the start failed. */
gboolean test_composition_root_restart (TestCompositionRoot *root, double seconds, BroBroError **error);
/* The library folder's volume goes away (its files are kept aside), and LibraryService checks at once (owner answer 10:
 * no OS event reports a library's volume); mount brings it back. FALSE when the check never ended within @seconds. */
gboolean test_composition_root_unmount (TestCompositionRoot *root, const char *path, double seconds);
gboolean test_composition_root_mount (TestCompositionRoot *root, const char *path, double seconds);

G_END_DECLS
