/* test-hosts.c: the hosts layer's tests: the real composition root's smoke test (decision 175), decision 301's twin (a key
 * a crash left in a job's home is scrubbed at startup) and the realDisc scenarios (test-real-disc.inc). A temporary data
 * folder and home, no system keystore, no drives, an empty environment, and but for a real disc run no system tools (so
 * nothing runs makemkvcon). Never the default data folder, the system keystore or the user's home. */
#include "bro-composition-root.h"
#include "bro-job-repository.h"
#include "bro-no-drives.h"
#include "bro-sqlite-store.h"
#include "bro-step-repository.h"
#include "bro-store.h"
#include "bro-system-clock.h"
#include "bro-test-kit.h"

#include <string.h>
#include <unistd.h>

/* An answer from the executor thread, waited for on the test's. */
typedef struct {
  GMutex lock;
  GCond cond;
  gboolean finished;
  char *error;   /* the error's code, NULL when it succeeded */
  char *version; /* system(): the facts it gave */
  char *platform;
  char *data_dir;
  gint64 pid;
  gboolean makemkv_found;
  int tools_found;
  GString *problems; /* system(): each active problem's code, then a space */
  guint count;       /* a list: how many */
  BroJsonValue *json; /* a job or a list of log lines, as JSON */
} Answer;

static void
answer_finish (Answer *a, const BroBroError *error)
{
  g_mutex_lock (&a->lock);
  a->error = error ? g_strdup (error->code) : NULL;
  a->finished = TRUE;
  g_cond_signal (&a->cond);
  g_mutex_unlock (&a->lock);
}

static void
answer_done (const BroBroError *error, gpointer data)
{
  answer_finish (data, error);
}

static void
answer_status (const BroSystemStatusDto *status, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (status)
    {
      a->version = g_strdup (status->facts->version);
      a->platform = g_strdup (status->facts->platform);
      a->data_dir = g_strdup (status->facts->data_dir);
      a->pid = status->facts->pid;
      a->makemkv_found = status->makemkv_found;
      for (guint i = 0; i < status->tools->len; i++)
        if (((BroToolInfo *) g_ptr_array_index (status->tools, i))->path)
          a->tools_found++;
      a->problems = g_string_new ("");
      for (guint i = 0; i < status->problems->len; i++)
        g_string_append_printf (a->problems, "%s ",
                                bro_message_code_wire (((BroProblemDto *) g_ptr_array_index (status->problems, i))->message->code));
    }
  answer_finish (a, error);
}

static void
answer_list (GPtrArray *items, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (items)
    a->count = items->len;
  answer_finish (a, error);
}

/* TRUE once the answer came (within @seconds). */
static gboolean
answer_wait (Answer *a, double seconds)
{
  gint64 until = g_get_monotonic_time () + (gint64) (seconds * G_USEC_PER_SEC);
  gboolean finished;
  g_mutex_lock (&a->lock);
  while (!a->finished && g_cond_wait_until (&a->cond, &a->lock, until))
    ;
  finished = a->finished;
  g_mutex_unlock (&a->lock);
  return finished;
}

static void
answer_clear (Answer *a)
{
  g_free (a->error);
  g_free (a->version);
  g_free (a->platform);
  g_free (a->data_dir);
  if (a->problems)
    g_string_free (a->problems, TRUE);
  g_clear_pointer (&a->json, bro_json_value_unref);
  memset (a, 0, sizeof *a);
}

static void
test_real_root_builds_starts_answers_and_stops (void)
{
  g_autoptr (GError) gerror = NULL;
  g_autofree char *root = g_dir_make_tmp ("bromelia-host-XXXXXX", &gerror);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autofree char *home = g_build_filename (root, "home", NULL);
  g_autofree char *db = g_build_filename (data, "bromelia.db", NULL);
  g_autofree char *secrets = g_build_filename (data, "secrets", NULL);
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (BroBroError) error = NULL;
  BroRootOptions options = { data, NULL, home, FALSE, FALSE, FALSE, environment };
  BroLocalEngine *engine;
  Answer a = { 0 };
  g_assert_no_error (gerror);
  g_mkdir_with_parents (home, 0700);

  engine = bro_composition_root_build (&options, &error);
  g_assert_null (error);
  g_assert_nonnull (engine);
  g_assert_true (g_file_test (db, G_FILE_TEST_IS_REGULAR)); /* the store is opened in the data folder */

  bro_local_engine_start (engine, answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);

  bro_engine_api_system (BRO_ENGINE_API (engine), answer_status, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  g_assert_cmpstr (a.version, ==, "1.0.0");
  g_assert_cmpstr (a.version, ==, BRO_COMPOSITION_ROOT_APP_VERSION);
  g_assert_cmpstr (a.platform, ==, "linux");
  g_assert_cmpstr (a.data_dir, ==, data);
  g_assert_cmpint (a.pid, ==, getpid ());
  g_assert_false (a.makemkv_found);
  g_assert_cmpint (a.tools_found, ==, 0);
  answer_clear (&a);

  bro_engine_api_stop (BRO_ENGINE_API (engine), answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 40));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);

  /* After stop every call answers server.stopping (on this thread: the executor has stopped). */
  bro_engine_api_system (BRO_ENGINE_API (engine), answer_status, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, "server.stopping");
  answer_clear (&a);
  g_object_unref (engine);

  if (g_file_test (secrets, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) dir = g_dir_open (secrets, 0, NULL);
      g_assert_null (dir ? g_dir_read_name (dir) : NULL); /* no secret was written */
    }
  bro_test_remove_tree (root);
}

/* Decision 301's twin on Linux (review L3-7): the key a crash left in an unfinished job's home is scrubbed by the real
 * root's startup. HomeDirIsolation's own cases and recovery-service's unreleased-homes-are-scrubbed-and-the-registry-
 * put-back pin the scrub itself; this pins that the root hands RecoveryService the same isolation and job homes
 * (<data>/jobs/<id>/home) MakemkvTool runs in. The job row is written through the SQLite store before the build, and the
 * settings.conf holds a test key only: never a real one. */
static void
test_a_key_a_crash_left_in_a_jobs_home_is_scrubbed_when_the_real_root_starts (void)
{
  g_autoptr (GError) gerror = NULL;
  g_autofree char *root = g_dir_make_tmp ("bromelia-host-XXXXXX", &gerror);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autofree char *home = g_build_filename (root, "home", NULL);
  g_autofree char *db = g_build_filename (data, "bromelia.db", NULL);
  BroId id = bro_test_counted_id (1);
  g_autofree char *job_folder = g_build_filename (data, "jobs", id.value, NULL);
  g_autofree char *folder = g_build_filename (job_folder, "home", ".MakeMKV", NULL);
  g_autofree char *conf = g_build_filename (folder, "settings.conf", NULL);
  g_autofree char *log_path = g_build_filename (job_folder, "log.jsonl", NULL);
  g_autofree char *before = g_strdup_printf ("app_DataDir = \"%s\"\napp_Key = \"T-BromeliaTest-not-a-key\"\napp_UpdateEnable = \"0\"\n", folder);
  g_autofree char *text = NULL;
  g_autofree char *log = NULL;
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (BroBroError) error = NULL;
  BroRootOptions options = { data, NULL, home, FALSE, FALSE, FALSE, environment };
  BroLocalEngine *engine;
  Answer a = { 0 };
  g_assert_no_error (gerror);
  g_mkdir_with_parents (home, 0700);
  g_mkdir_with_parents (folder, 0700);
  g_assert_true (g_file_set_contents (conf, before, -1, NULL));

  {
    g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
    BroSqliteStore *store = bro_sqlite_store_open (db, BRO_CLOCK (clock), &error);
    g_autoptr (BroJobRecord) job = bro_job_record_new ();
    g_autoptr (BroStepRecord) row = bro_step_record_new ();
    BroInstant now = bro_clock_now (BRO_CLOCK (clock));
    g_assert_null (error);
    g_assert_true (bro_sqlite_store_migrate (store, &error));
    job->id = id;
    job->kind = BRO_JOB_KIND_VIDEO_DISC;
    job->state = BRO_JOB_STATE_RUNNING;
    job->queue = BRO_QUEUE_ACQUISITION;
    job->title = g_strdup ("Sample Movie");
    job->request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, "Sample Movie");
    job->created_at = now;
    g_assert_true (bro_job_repository_insert (bro_store_jobs (BRO_STORE (store)), job, &error));
    row->job_id = id;
    row->seq = 0;
    row->kind = BRO_STEP_KIND_PROBE;
    row->state = BRO_STEP_STATE_FAILED;
    row->attempt = 1;
    row->has_started_at = row->has_finished_at = TRUE;
    row->started_at = row->finished_at = now;
    g_assert_true (bro_step_repository_save (bro_store_steps (BRO_STORE (store)), row, &error));
    bro_sqlite_store_close (store);
    g_object_unref (store);
  }

  engine = bro_composition_root_build (&options, &error);
  g_assert_null (error);
  g_assert_nonnull (engine);
  bro_local_engine_start (engine, answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);

  g_assert_true (g_file_get_contents (conf, &text, NULL, NULL));
  g_assert_null (strstr (text, "app_Key"));                       /* the key is scrubbed */
  g_assert_nonnull (strstr (text, "app_UpdateEnable = \"0\"")); /* the other settings stay */
  /* No makemkv.keyNotRemoved, raised or in the job's log. */
  bro_engine_api_system (BRO_ENGINE_API (engine), answer_status, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  g_assert_null (strstr (a.problems->str, "makemkv.keyNotRemoved"));
  answer_clear (&a);
  if (g_file_get_contents (log_path, &log, NULL, NULL))
    g_assert_null (strstr (log, "makemkv.keyNotRemoved"));

  bro_engine_api_stop (BRO_ENGINE_API (engine), answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 40));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);
  g_object_unref (engine);
  bro_test_remove_tree (root);
}

/* I1-5: without drives nothing scans one. With no tools the real scan fails, and start waits for the first scan, so
 * makemkv.driveScanFailed would be active here. */
static void
test_no_drive_scan_runs_without_drives (void)
{
  g_autoptr (GError) gerror = NULL;
  g_autofree char *root = g_dir_make_tmp ("bromelia-host-XXXXXX", &gerror);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autofree char *home = g_build_filename (root, "home", NULL);
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (BroBroError) error = NULL;
  BroRootOptions options = { data, NULL, home, FALSE, FALSE, FALSE, environment };
  BroLocalEngine *engine;
  Answer a = { 0 };
  g_assert_no_error (gerror);
  g_mkdir_with_parents (home, 0700);

  engine = bro_composition_root_build (&options, &error);
  g_assert_null (error);
  bro_local_engine_start (engine, answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);

  bro_engine_api_system (BRO_ENGINE_API (engine), answer_status, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  g_assert_cmpstr (a.problems->str, ==, "");
  answer_clear (&a);

  bro_engine_api_drives (BRO_ENGINE_API (engine), answer_list, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  g_assert_cmpint (a.count, ==, 0);
  answer_clear (&a);

  bro_engine_api_stop (BRO_ENGINE_API (engine), answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 40));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);
  g_object_unref (engine);
  bro_test_remove_tree (root);
}

static void
no_drives_heard (const BroDeviceEvent *event, gpointer data)
{
  (void) event;
  *(int *) data += 1;
}

/* I1-5: with no drives the monitor reports nothing and every drive command is refused. */
static void
test_no_drives_report_nothing_and_refuse_everything (void)
{
  g_autoptr (BroDeviceMonitor) monitor = bro_no_device_monitor_new ();
  g_autoptr (BroDriveControl) control = bro_no_drive_control_new ();
  g_autoptr (GPtrArray) drives = NULL;
  g_autoptr (BroBroError) ejected = NULL;
  g_autoptr (BroBroError) closed = NULL;
  g_autoptr (BroBroError) opened = NULL;
  int heard = 0;

  bro_device_monitor_start (monitor, no_drives_heard, &heard, NULL);
  drives = bro_device_monitor_current_drives (monitor);
  g_assert_cmpuint (drives->len, ==, 0);
  bro_device_monitor_stop (monitor);
  g_assert_cmpint (heard, ==, 0);

  g_assert_false (bro_drive_control_eject (control, "/dev/x", &ejected));
  g_assert_cmpstr (ejected->code, ==, "drive.notFound");
  g_assert_false (bro_drive_control_close_tray (control, "/dev/x", &closed));
  g_assert_cmpstr (closed->code, ==, "drive.notFound");
  g_assert_null (bro_drive_control_open_raw (control, "/dev/x", &opened));
  g_assert_cmpstr (opened->code, ==, "drive.notFound");
  g_assert_null (bro_drive_control_wait_for_mount (control, "/dev/x", (BroDuration) { 1 }, NULL));
  g_assert_cmpint (bro_drive_control_probe_content (control, "/dev/x"), ==, BRO_DISC_CONTENT_UNKNOWN);
}

#include "test-real-disc.inc"

int
main (int argc, char **argv)
{
  g_autoptr (GPtrArray) ids = NULL;
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/hosts/real-root-builds-starts-answers-and-stops", test_real_root_builds_starts_answers_and_stops);
  g_test_add_func ("/hosts/a-key-a-crash-left-in-a-jobs-home-is-scrubbed-when-the-real-root-starts",
                   test_a_key_a_crash_left_in_a_jobs_home_is_scrubbed_when_the_real_root_starts);
  g_test_add_func ("/hosts/no-drive-scan-runs-without-drives", test_no_drive_scan_runs_without_drives);
  g_test_add_func ("/hosts/no-drives-report-nothing-and-refuse-everything", test_no_drives_report_nothing_and_refuse_everything);
  ids = real_disc_add_tests ();
  return g_test_run ();
}
