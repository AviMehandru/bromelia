/* test-full-disk.inc: what a full disk does to Bromelia's writes (review follow-ups, failure testing), included by
 * test-adapters.c after test-crash.inc. Opt-in: BROMELIA_TEST_FULL_DIR names an empty folder on a small volume of its
 * own (a 32 MB disk image on macOS, a tmpfs with size= on Linux); each test fills it around the step it checks. */
#include <sys/statvfs.h>

static const char *
full_dir (void)
{
  const char *dir = g_getenv ("BROMELIA_TEST_FULL_DIR");
  if (!dir || !*dir)
    g_test_skip ("set BROMELIA_TEST_FULL_DIR to an empty folder on a small volume of its own");
  return dir && *dir ? dir : NULL;
}

static guint64
free_bytes (const char *dir)
{
  struct statvfs v;
  g_assert_cmpint (statvfs (dir, &v), ==, 0);
  return (guint64) v.f_bavail * v.f_frsize;
}

/* Fills the volume until at most @leave bytes are free; returns the filler's path (remove it afterwards). */
static char *
fill (const char *dir, guint64 leave)
{
  char *filler = g_build_filename (dir, "filler.bin", NULL);
  static char block[64 * 1024];
  int fd = open (filler, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  g_assert_cmpint (fd, >=, 0);
  while (free_bytes (dir) > leave)
    {
      gsize n = MIN (sizeof block, free_bytes (dir) - leave);
      if (write (fd, block, n) <= 0)
        break;
      fsync (fd);
    }
  close (fd);
  return filler;
}

/* Every name in @dir but the filler. */
static char *
names_in (const char *dir)
{
  GString *s = g_string_new (NULL);
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  while ((name = g_dir_read_name (d)) != NULL)
    if (strcmp (name, "filler.bin") != 0)
      g_string_append_printf (s, "%s%s", s->len ? "," : "", name);
  g_dir_close (d);
  return g_string_free (s, FALSE);
}

/* A data disc bigger than the free space: the copy fails and leaves nothing, never a partial disc.iso. */
static void
test_full_disk_data_imager (void)
{
  const char *dir = full_dir ();
  g_autoptr (TestFakeDriveControl) drives = NULL;
  g_autoptr (BroPlatformFileSystem) fs = NULL;
  g_autoptr (TestRecordingSink) sink = NULL;
  g_autoptr (BroCancellationSource) cancel = NULL;
  g_autoptr (BroDataImager) imager = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroDataImageCopy) copy = NULL;
  g_autofree char *dest = NULL, *left = NULL;
  if (!dir)
    return;
  drives = g_object_new (TEST_TYPE_FAKE_DRIVE_CONTROL, NULL);
  fs = bro_platform_file_system_new ();
  sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  cancel = bro_cancellation_source_new ();
  imager = bro_data_imager_new (BRO_DRIVE_CONTROL (drives), BRO_FILE_SYSTEM (fs));
  drives->disc = g_object_new (TEST_TYPE_FAKE_DISC, NULL);
  drives->disc->sectors = (gint64) (free_bytes (dir) / 2048) + 4096;
  dest = g_build_filename (dir, "disc.iso", NULL);
  copy = bro_data_imager_copy (imager, "/dev/sr0", dest, BRO_RUN_SINK (sink), bro_cancellation_source_token (cancel), &error);
  g_assert_null (copy);
  g_assert_nonnull (error);
  g_test_message ("full disk: %s %s", error->code, bro_json_value_get_string (bro_json_value_member (error->params, "reason"), ""));
  left = names_in (dir);
  g_assert_cmpstr (left, ==, "");
}

/* A full disk under writeAtomically: it fails, the old file is still whole, no temporary file is left. */
static void
test_full_disk_write_atomically (void)
{
  const char *dir = full_dir ();
  g_autoptr (BroPlatformFileSystem) fs = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GBytes) old = NULL, big = NULL;
  g_autofree char *path = NULL, *filler = NULL, *text = NULL, *left = NULL;
  g_autofree guint8 *data = NULL;
  if (!dir)
    return;
  fs = bro_platform_file_system_new ();
  path = g_build_filename (dir, "r.json", NULL);
  old = g_bytes_new_static ("old", 3);
  g_assert_true (bro_platform_file_system_write_atomically (fs, path, old, 0644, &error));
  filler = fill (dir, 64 * 1024);
  data = g_malloc0 (1 << 20);
  big = g_bytes_new_static (data, 1 << 20);
  g_assert_false (bro_platform_file_system_write_atomically (fs, path, big, 0644, &error));
  g_test_message ("full disk: %s %s", error->code, bro_json_value_get_string (bro_json_value_member (error->params, "reason"), ""));
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "old");
  left = names_in (dir);
  g_assert_cmpstr (left, ==, "r.json");
  g_unlink (filler);
  g_unlink (path);
}

/* A transcript that fills the disk: the tool runs to the end, every line still reaches the reader, and the run
 * reports process.noTranscript. */
static void
test_full_disk_transcript (void)
{
  const char *dir = full_dir ();
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroRunningProcess) p = NULL;
  g_autoptr (BroBroMessage) problem = NULL;
  g_autofree char *filler = NULL, *transcript = NULL;
  BroProcessExit exit;
  if (!dir)
    return;
  spec = spec_new ("/bin/sh", "-c", "i=0; while [ $i -lt 20000 ]; do echo line-$i-0123456789012345678901234567890123456789; i=$((i+1)); done",
                   NULL);
  filler = fill (dir, 32 * 1024);
  transcript = g_build_filename (dir, "t.txt", NULL);
  spec->transcript = g_strdup (transcript);
  p = start (spec);
  exit = run (p, lines);
  g_assert_cmpint (exit.status, ==, 0);
  g_assert_cmpuint (lines->len, ==, 20000);
  problem = bro_running_process_transcript_problem (p);
  g_assert_nonnull (problem);
  g_assert_cmpstr (bro_message_code_wire (problem->code), ==, "process.noTranscript");
  g_unlink (filler);
  g_unlink (transcript);
}

/* The database on a full disk: a write fails with store.failed, and once there is space again the database opens, is
 * intact, and still has what was written before. */
static void
test_full_disk_store (void)
{
  const char *dir = full_dir ();
  g_autoptr (TestFixedClock) clock = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *path = NULL, *filler = NULL, *pad = NULL;
  BroSqliteStore *store;
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  gboolean failed = FALSE;
  if (!dir)
    return;
  clock = g_object_new (TEST_TYPE_FIXED_CLOCK, NULL);
  path = g_build_filename (dir, "s.sqlite", NULL);
  store = bro_sqlite_store_open (path, BRO_CLOCK (clock), &error);
  g_assert_nonnull (store);
  g_assert_true (bro_sqlite_store_migrate (store, &error));
  {
    g_autoptr (BroJsonValue) kept = bro_json_value_new_string ("kept");
    g_assert_true (bro_key_value_repository_set (bro_store_kv (BRO_STORE (store)), "before", kept, &error));
  }
  filler = fill (dir, 48 * 1024);
  pad = g_strnfill (8000, 'x');
  for (int i = 0; i < 2000 && !failed; i++)
    {
      g_autofree char *key = g_strdup_printf ("k%d", i);
      g_autoptr (BroJsonValue) value = bro_json_value_new_string (pad);
      failed = !bro_key_value_repository_set (bro_store_kv (BRO_STORE (store)), key, value, &error);
    }
  g_assert_true (failed);
  g_test_message ("full disk: %s %s", error->code, bro_json_value_get_string (bro_json_value_member (error->params, "reason"), ""));
  g_assert_cmpstr (error->code, ==, "store.failed");
  g_object_unref (store);
  g_unlink (filler);
  g_clear_pointer (&error, bro_bro_error_free);
  store = bro_sqlite_store_open (path, BRO_CLOCK (clock), &error);
  g_assert_nonnull (store);
  {
    g_autoptr (BroJsonValue) v = bro_key_value_repository_get (bro_store_kv (BRO_STORE (store)), "before", &error);
    g_assert_cmpstr (bro_json_value_get_string (v, NULL), ==, "kept");
  }
  g_object_unref (store);
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_prepare_v2 (db, "PRAGMA integrity_check", -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (stmt), ==, SQLITE_ROW);
  g_assert_cmpstr ((const char *) sqlite3_column_text (stmt, 0), ==, "ok");
  sqlite3_finalize (stmt);
  sqlite3_close (db);
  remove_tree (dir);
  g_mkdir_with_parents (dir, 0755);
}

static gpointer
unmount_later (gpointer command)
{
  g_usleep (2 * G_USEC_PER_SEC);
  g_spawn_command_line_sync (command, NULL, NULL, NULL, NULL);
  return NULL;
}

/* A share that goes away during a copy (opt-in: BROMELIA_TEST_UNMOUNT_DIR, a folder on a mounted share, and
 * BROMELIA_TEST_UNMOUNT_CMD, the command that unmounts it or stops its server, run 2 s into the copy): the copy fails
 * with fs.failed, or fs.notFound when the folder is gone by the rename (a lazy unmount on Linux leaves open files
 * writable), never succeeds. How long it takes is reported, not checked: when a CIFS server stops, Linux holds the
 * write (state D, even on a soft mount) until the server is back, then fails it. */
static void
test_share_gone_data_imager (void)
{
  const char *dir = g_getenv ("BROMELIA_TEST_UNMOUNT_DIR"), *command = g_getenv ("BROMELIA_TEST_UNMOUNT_CMD");
  g_autoptr (TestFakeDriveControl) drives = NULL;
  g_autoptr (BroPlatformFileSystem) fs = NULL;
  g_autoptr (TestRecordingSink) sink = NULL;
  g_autoptr (BroCancellationSource) cancel = NULL;
  g_autoptr (BroDataImager) imager = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroDataImageCopy) copy = NULL;
  g_autofree char *dest = NULL;
  GThread *later;
  gint64 t0;
  if (!dir || !*dir || !command || !*command)
    {
      g_test_skip ("set BROMELIA_TEST_UNMOUNT_DIR and BROMELIA_TEST_UNMOUNT_CMD");
      return;
    }
  drives = g_object_new (TEST_TYPE_FAKE_DRIVE_CONTROL, NULL);
  fs = bro_platform_file_system_new ();
  sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  cancel = bro_cancellation_source_new ();
  imager = bro_data_imager_new (BRO_DRIVE_CONTROL (drives), BRO_FILE_SYSTEM (fs));
  drives->disc = g_object_new (TEST_TYPE_FAKE_DISC, NULL);
  drives->disc->sectors = 200 * 512;
  drives->disc->delay_us = 50000;
  dest = g_build_filename (dir, "disc.iso", NULL);
  later = g_thread_new ("unmount", unmount_later, (gpointer) command);
  t0 = g_get_monotonic_time ();
  copy = bro_data_imager_copy (imager, "/dev/sr0", dest, BRO_RUN_SINK (sink), bro_cancellation_source_token (cancel), &error);
  g_thread_join (later);
  g_assert_null (copy);
  g_assert_nonnull (error);
  g_test_message ("share gone: %s %s after %.1f s", error->code, bro_json_value_get_string (bro_json_value_member (error->params, "reason"), ""),
                  since (t0));
  g_assert_true (g_str_equal (error->code, "fs.failed") || g_str_equal (error->code, "fs.notFound"));
}
