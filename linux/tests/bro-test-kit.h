/* bro-test-kit.h: the test kit (rearchitecture plan §17), fakes of the ports for the adapter tests and the test
 * composition root: TestScriptedLauncher (a BroProcessLauncher whose TestScript processes play a script),
 * TestRecordingIsolation (a BroSettingsIsolation that records what happens to its TestRecordingLease leases) and
 * TestFixedLocator (a BroToolLocator with fixed paths). Test code, not shipped. */
#pragma once

#include "bro-cancellation-source.h"
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
};

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
  GPtrArray *started; /* BroProcessSpec * (copies of what matters) */
  TestScript *last;
};

/* An isolation that records what happens to its leases. */
#define TEST_TYPE_RECORDING_ISOLATION (test_recording_isolation_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingIsolation, test_recording_isolation, TEST, RECORDING_ISOLATION, GObject)

struct _TestRecordingIsolation {
  GObject parent_instance;
  GPtrArray *log;
  char *release_problem; /* the reason release reports in makemkv.keyNotRemoved, or NULL */
  gboolean warns;        /* the leases' warning is makemkv.guiRunning */
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
};

/* Deletes a file or a folder with everything in it; a missing path is no error. */
void bro_test_remove_tree (const char *path);

G_END_DECLS
