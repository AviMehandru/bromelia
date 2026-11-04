/* bro-test-kit.h: the test kit (rearchitecture plan §17), fakes of the ports for the adapter and engine tests and the
 * test composition root: TestScriptedLauncher (a BroProcessLauncher whose TestScript processes play a script),
 * TestRecordingIsolation (a BroSettingsIsolation that records what happens to its TestRecordingLease leases),
 * TestFixedLocator (a BroToolLocator with fixed paths), TestManualClock (a BroClock that moves only when told),
 * TestMemoryFs (files and folders in memory), TestIds (ids that count), TestFakeDrives (the OS's drives: a
 * BroDeviceMonitor and BroDriveControl), TestMemoryKeystore (secrets in memory) and TestRecordingEngineLog (a
 * BroEngineLog that keeps its lines). Test code, not shipped. */
#pragma once

#include "bro-cancellation-source.h"
#include "bro-clock.h"
#include "bro-device-monitor.h"
#include "bro-drive-control.h"
#include "bro-engine-log.h"
#include "bro-file-system.h"
#include "bro-id-generator.h"
#include "bro-keystore.h"
#include "bro-process-launcher.h"
#include "bro-settings-isolation.h"
#include "bro-tool-locator.h"

G_BEGIN_DECLS

/* A process that plays a script: the lines, one per call to lines(); then the exit status, after creating a file in
 * the destination. A stop ends it at once with status -1 and signal 15. */
#define TEST_TYPE_SCRIPT (test_script_get_type ())
G_DECLARE_FINAL_TYPE (TestScript, test_script, TEST, SCRIPT, GObject)

struct _TestScript {
  GObject parent_instance;
  GStrv lines;
  GArray *stderr_at; /* int: the indexes of the lines that come on stderr (nullable) */
  GStrv write_files;     /* written in write_file_in (NULL: title_t00.mkv); a name with / makes its folders, "" writes
                          * write_file_in itself as a file */
  gboolean stalls;       /* after the lines: end as stopped for silence */
  double stall_seconds;  /* the spec's */
  int exit_code;
  char *write_file_in;
  int cancel_after;
  int delete_after;     /* after this many lines, delete delete_dir (-1: never) */
  char *delete_dir;
  BroCancellationSource *cancel;
  GStrv arguments;      /* the spec's */
  int split_parts;      /* create this many parts from the -o argument's %03d pattern */
  int write_index;      /* write write_text to the file named by this argument (-1: none) */
  char *write_text;
  char *transcript_fails_at; /* report process.noTranscript for this path (nullable) */
  int handed;
  gboolean stopped;
  BroStopReason reason;
  BroProcessExit exit;
  /* For the test root (decision 193): */
  BroFileSystem *files;      /* where it writes its output files (nullable: real disk) */
  gboolean holds;            /* after its lines it runs until it is stopped */
  double stop_takes;         /* seconds a stopped process takes to end */
  gboolean ended;
  struct _TestLaunchCounts *counts; /* the launcher's (nullable) */
  GMutex lock;               /* stopped, reason, ended */
  GCond stop_cond;
};

/* The lines for a spec (a strv the launcher frees; NULL: the launcher's lines). Any thread. */
typedef GStrv (*TestLinesFunc) (const BroProcessSpec *spec, gpointer data);
/* Whether a spec runs until it is stopped after its lines (a rip in progress). Any thread. */
typedef gboolean (*TestHoldsFunc) (const BroProcessSpec *spec, gpointer data);

#define TEST_TYPE_SCRIPTED_LAUNCHER (test_scripted_launcher_get_type ())
G_DECLARE_FINAL_TYPE (TestScriptedLauncher, test_scripted_launcher, TEST, SCRIPTED_LAUNCHER, GObject)

struct _TestScriptedLauncher {
  GObject parent_instance;
  GStrv lines;
  GArray *stderr_at; /* int (nullable) */
  GStrv write_files; /* NULL: title_t00.mkv */
  gboolean stalls;
  int exit_code;
  char *write_file_in;
  int cancel_after;
  int delete_after;
  char *delete_dir;
  BroCancellationSource *cancel;
  int split_parts;
  int write_index; /* -1: none */
  char *write_text;
  gboolean transcript_fails; /* the process reports process.noTranscript */
  GPtrArray *started; /* BroProcessSpec * (copies of what matters); under lock once processes start on several threads */
  TestScript *last;
  /* For the test root (decision 193), set before the first start: */
  BroFileSystem *files;  /* where the processes write their output files (NULL: real disk); a reference */
  TestLinesFunc lines_for;
  gpointer lines_for_data;
  TestHoldsFunc runs_until_stopped;
  gpointer runs_until_stopped_data;
  double stop_takes; /* seconds a stopped process takes to end (a process slow to die); real time */
  struct _TestLaunchCounts *counts;
  GMutex lock;
};

/* The processes waiting to be stopped after their lines (runs_until_stopped): each is a step a harness knows is
 * parked. */
int test_scripted_launcher_held (TestScriptedLauncher *self);
/* The processes that haven't ended yet. */
int test_scripted_launcher_running (TestScriptedLauncher *self);
/* Copies of the specs started so far (BroProcessSpec *; any thread). */
GPtrArray *test_scripted_launcher_specs (TestScriptedLauncher *self);

/* An isolation that records what happens to its leases. */
#define TEST_TYPE_RECORDING_ISOLATION (test_recording_isolation_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingIsolation, test_recording_isolation, TEST, RECORDING_ISOLATION, GObject)

struct _TestRecordingIsolation {
  GObject parent_instance;
  GPtrArray *log;
  char *release_problem; /* the reason release reports in makemkv.keyNotRemoved, or NULL */
  gboolean warns;        /* the leases' warning is makemkv.guiRunning */
  GMutex lock;           /* log, as leases are prepared and released on several threads */
};

#define TEST_TYPE_RECORDING_LEASE (test_recording_lease_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingLease, test_recording_lease, TEST, RECORDING_LEASE, GObject)

struct _TestRecordingLease {
  GObject parent_instance;
  TestRecordingIsolation *owner;
  char *home;
  char *profile;
};

/* Finds makemkvcon at path (NULL: nowhere). */
#define TEST_TYPE_FIXED_LOCATOR (test_fixed_locator_get_type ())
G_DECLARE_FINAL_TYPE (TestFixedLocator, test_fixed_locator, TEST, FIXED_LOCATOR, GObject)

struct _TestFixedLocator {
  GObject parent_instance;
  char *path;    /* makemkvcon */
  char *apprise;
  char *mkvmerge;
  char *mkvextract;
  char *handbrake;
  char *makemkvcon_version; /* what it reports as makemkvcon's version (nullable) */
};

/* Deletes a file or a folder with everything in it; a missing path is no error. */
void bro_test_remove_tree (const char *path);

/* ---- a clock that moves only when told (the scenarios' fake clock), moved from test-engine.c ---------------------- */

/* 2026-10-02T20:00:00.000Z, where it starts. */
#define TEST_MANUAL_CLOCK_START_MS G_GINT64_CONSTANT (1790971200000)

/* Its local time is UTC plus utc_offset_minutes (0 unless set; no daylight saving time). Any thread. */
#define TEST_TYPE_MANUAL_CLOCK (test_manual_clock_get_type ())
G_DECLARE_FINAL_TYPE (TestManualClock, test_manual_clock, TEST, MANUAL_CLOCK, GObject)

struct _TestManualClock {
  GObject parent_instance;
  GMutex lock;
  gint64 now_ms;
  int utc_offset_minutes;
  guint64 made;
  GPtrArray *timers; /* of the clock's pending timers, references */
};

/* Fires each due timer in due order (ties in the order they were made), with the time set to its due time, then
 * calls @settle. */
void test_manual_clock_advance (TestManualClock *self, gint64 ms, void (*settle) (gpointer), gpointer settle_data);
/* The timers not cancelled yet. */
int test_manual_clock_pending (TestManualClock *self);
/* When the next timer not cancelled is due (into @due); FALSE when there is none. */
gboolean test_manual_clock_next_due (TestManualClock *self, BroInstant *due);
/* Cancels every timer (a killed engine's: none of them fires any more). */
void test_manual_clock_cancel_all (TestManualClock *self);

/* ---- files and folders in memory, moved from test-pipeline.inc ------------------------------------------------------ */

/* Hears of a whole-file read before it reads (a read that is slow, say). Any thread. */
typedef void (*TestReadFunc) (const char *path, gpointer data);

/* Hears of a file operation (@op, @path) before it acts. FALSE with @error set: the call fails with it. Any thread. */
typedef gboolean (*TestOpFunc) (const char *op, const char *path, gpointer data, BroBroError **error);

/* Hears of a rename (by its from path) once it has landed, outside the lock (a scenario's crash points). FALSE with @error
 * set: the call fails with it. Any thread. */
typedef gboolean (*TestRenamedFunc) (const char *from, gpointer data, BroBroError **error);

/* What the engine's tests need of the FileSystem port. Thread-safe. test_memory_fs_fail makes an operation on a path
 * fail before it acts (open, read, sync, syncDirectory, write, remove, rename: its from path); a rename refuses a path
 * that exists and a folder that isn't there, as the real ones do; on_op and on_renamed hear of the operations.
 * move_merging moves as the platform file systems do (a
 * folder merges into a folder of the same name, ASCII case ignored; anything else takes ConflictNamer's name); move_to_trash
 * isn't faked. While test_memory_fs_freeze holds, every call that changes something (write, rename, remove,
 * createDirectory, sync, syncDirectory) fails (fs.failed, reason "frozen"): a killed engine's writes never land;
 * test_memory_fs_put and test_memory_fs_put_folder still work. */
#define TEST_TYPE_MEMORY_FS (test_memory_fs_get_type ())
G_DECLARE_FINAL_TYPE (TestMemoryFs, test_memory_fs, TEST, MEMORY_FS, GObject)

struct _TestMemoryFs {
  GObject parent_instance;
  GMutex lock;
  GHashTable *files;      /* path → GBytes */
  GHashTable *folders;    /* paths */
  GHashTable *read_fails; /* path → BroBroError: whole-file reads of these fail so */
  GPtrArray *fails;       /* "op\npath" → see test_memory_fs_fail */
  GPtrArray *fail_errors; /* BroBroError *, one per fails entry */
  BroVolumeInfo *any_volume; /* what volume gives for any path (NULL: fs.notFound); set before use */
  TestReadFunc on_read;      /* set before use */
  gpointer on_read_data;
  TestOpFunc on_op;          /* set before use */
  TestRenamedFunc on_renamed; /* set before use */
  gpointer on_op_data;       /* both hooks' */
  gint frozen;               /* atomic */
};

/* A file with these bytes, its folders made. */
void test_memory_fs_put (TestMemoryFs *self, const char *path, const void *bytes, gsize length);
void test_memory_fs_put_folder (TestMemoryFs *self, const char *path);
/* Every call that changes something fails from now on, until thaw. */
void test_memory_fs_freeze (TestMemoryFs *self);
void test_memory_fs_thaw (TestMemoryFs *self);
gboolean test_memory_fs_frozen (TestMemoryFs *self);
/* @op on @path fails with @error (copied) from now on. */
void test_memory_fs_fail (TestMemoryFs *self, const char *op, const char *path, const BroBroError *error);
/* No operation fails any more (read_fails stay). */
void test_memory_fs_unfail (TestMemoryFs *self);
/* Whole-file reads of @path fail with @error (copied). */
void test_memory_fs_fail_reads (TestMemoryFs *self, const char *path, const BroBroError *error);

/* ---- ids that count: 00000000-0000-4000-8000-000000000001, ... after n, moved from test-pipeline.inc ----------------- */

#define TEST_TYPE_IDS (test_ids_get_type ())
G_DECLARE_FINAL_TYPE (TestIds, test_ids, TEST, IDS, GObject)

struct _TestIds {
  GObject parent_instance;
  gint n; /* the last one given */
};

/* 00000000-0000-4000-8000-<n as 12 hex digits>. */
BroId bro_test_counted_id (int n);

/* ---- the operating system's drives ---------------------------------------------------------------------------------- */

/* What current_drives lists, the events test_fake_drives_send reports to the monitor's sink, the content probe_content
 * finds for a device (video unless test_fake_drives_set_content says otherwise), and the tray calls ("eject <device>",
 * "closeTray <device>"). A mount wait answers the drive's mount path at once. Raw sector reads aren't faked. The tray
 * callbacks (test_fake_drives_on_tray) hear of each tray call after it is recorded (a scenario's drive world changes with
 * it). Thread-safe. */
#define TEST_TYPE_FAKE_DRIVES (test_fake_drives_get_type ())
G_DECLARE_FINAL_TYPE (TestFakeDrives, test_fake_drives, TEST, FAKE_DRIVES, GObject)

/* A drive the OS lists from now on (copied). */
void test_fake_drives_add (TestFakeDrives *self, const BroOsDrive *drive);
/* What probe_content finds on @device (the one the engine names: MakeMKV's). */
void test_fake_drives_set_content (TestFakeDrives *self, const char *device, BroDiscContent content);
/* Reports @event to the monitor's sink (nothing before start or after stop). */
void test_fake_drives_send (TestFakeDrives *self, const BroDeviceEvent *event);
/* Whether the monitor is started (a sink is kept). */
gboolean test_fake_drives_monitoring (TestFakeDrives *self);
/* The tray calls so far (char *; free the array). */
GPtrArray *test_fake_drives_calls (TestFakeDrives *self);
/* Hears of a tray call with its device, outside the lock, on the caller's thread. */
typedef void (*TestTrayFunc) (const char *device, gpointer data);
/* @on_eject and @on_close_tray (each nullable) hear of each eject and tray close the engine asks for; set before start. */
void test_fake_drives_on_tray (TestFakeDrives *self, TestTrayFunc on_eject, TestTrayFunc on_close_tray, gpointer data);

/* ---- secrets in memory ---------------------------------------------------------------------------------------------- */

/* The test root's keystore pair (the "system" store and the secrets folder). Thread-safe. */
#define TEST_TYPE_MEMORY_KEYSTORE (test_memory_keystore_get_type ())
G_DECLARE_FINAL_TYPE (TestMemoryKeystore, test_memory_keystore, TEST, MEMORY_KEYSTORE, GObject)

TestMemoryKeystore *test_memory_keystore_new (gboolean system_store);
/* Calls about @name fail with @error (copied; NULL: they work again) before they act. */
void test_memory_keystore_fails (TestMemoryKeystore *self, const char *name, const BroBroError *error);

/* ---- an engine log that keeps its lines ------------------------------------------------------------------------------ */

#define TEST_TYPE_RECORDING_ENGINE_LOG (test_recording_engine_log_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingEngineLog, test_recording_engine_log, TEST, RECORDING_ENGINE_LOG, GObject)

/* Copies of the lines so far (BroBroMessage *; free the array). */
GPtrArray *test_recording_engine_log_lines (TestRecordingEngineLog *self);

G_END_DECLS
