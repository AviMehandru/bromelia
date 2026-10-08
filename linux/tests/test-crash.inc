/* test-crash.inc: what a crash (SIGKILL) leaves behind, included by test-adapters.c after test-data-imager.inc
 * (review follow-ups, crash points; Phase 3's RecoveryService builds on it). Each test runs itself again as a
 * subprocess that kills itself at a chosen point; the parent then looks at the folder. Parameters go through the
 * environment, since the subprocess makes scratch folders of its own. */

/* A data disc copy killed after two of its four chunks are written: the hidden .part file is left, never disc.iso. */
static void
test_crash_data_imager (void)
{
  const char *dir = g_getenv ("BROMELIA_CRASH_DIR");
  if (g_test_subprocess ())
    {
      g_autoptr (TestFakeDriveControl) drives = g_object_new (TEST_TYPE_FAKE_DRIVE_CONTROL, NULL);
      g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
      g_autoptr (TestRecordingSink) sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
      g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
      g_autoptr (BroDataImager) imager = bro_data_imager_new (BRO_DRIVE_CONTROL (drives), BRO_FILE_SYSTEM (fs));
      g_autofree char *dest = g_build_filename (dir, "disc.iso", NULL);
      drives->disc = g_object_new (TEST_TYPE_FAKE_DISC, NULL);
      drives->disc->sectors = 4 * 512;
      drives->disc->kill_after = 3;
      bro_data_imager_copy (imager, "/dev/sr0", dest, BRO_RUN_SINK (sink), bro_cancellation_source_token (cancel), NULL);
      g_assert_not_reached ();
    }
  {
    g_autofree char *where = g_build_filename (scratch, "crash-image", NULL);
    GDir *d;
    const char *name;
    guint parts = 0, others = 0;
    remove_tree (where);
    g_mkdir_with_parents (where, 0755);
    g_setenv ("BROMELIA_CRASH_DIR", where, TRUE);
    g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed ();
    d = g_dir_open (where, 0, NULL);
    while ((name = g_dir_read_name (d)) != NULL)
      {
        if (g_str_has_prefix (name, ".disc.iso.part-"))
          parts++;
        else
          others++;
        g_assert_cmpstr (name, !=, "disc.iso");
      }
    g_dir_close (d);
    g_assert_cmpuint (parts, ==, 1);
    g_assert_cmpuint (others, ==, 0);
    remove_tree (where);
  }
}

static void
crash_on_moved (const BroMovedItem *item, gpointer data)
{
  static int seen;
  int *at = data, fd;
  g_autofree char *line = g_strdup_printf ("%s\t%s\n", item->from, item->to);
  gboolean before = g_strcmp0 (g_getenv ("BROMELIA_CRASH_WHEN"), "before") == 0;
  seen++;
  if (seen == *at && before)
    kill (getpid (), SIGKILL);
  fd = open (g_getenv ("BROMELIA_CRASH_REPORT"), O_WRONLY | O_APPEND | O_CREAT, 0644);
  g_assert_cmpint (write (fd, line, strlen (line)), ==, (gssize) strlen (line));
  close (fd);
  if (seen == *at)
    kill (getpid (), SIGKILL);
}

/* moveMerging killed around its third report: whatever it reported has moved, and at most the one item it was about
 * to report moved without a report (killed between the rename and onMoved). */
static void
test_crash_move_merging (void)
{
  const char *dir = g_getenv ("BROMELIA_CRASH_DIR");
  if (g_test_subprocess ())
    {
      g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
      g_autofree char *from = g_build_filename (dir, "from", NULL), *to = g_build_filename (dir, "to", NULL);
      int at = 3;
      bro_file_system_move_merging (BRO_FILE_SYSTEM (fs), from, to, BRO_MOVE_POLICY_NEVER_REPLACE, crash_on_moved, &at, NULL);
      g_assert_not_reached ();
    }
  for (int when = 0; when < 2; when++)
    {
      g_autofree char *where = g_build_filename (scratch, "crash-move", NULL);
      g_autofree char *from = g_build_filename (where, "from", NULL), *to = g_build_filename (where, "to", NULL);
      g_autofree char *report = g_build_filename (where, "report.txt", NULL), *text = NULL;
      g_auto (GStrv) lines = NULL;
      guint reported = 0, moved = 0;
      remove_tree (where);
      g_mkdir_with_parents (from, 0755);
      g_mkdir_with_parents (to, 0755);
      for (int i = 0; i < 8; i++)
        {
          g_autofree char *base = g_strdup_printf ("f%d.mkv", i), *path = g_build_filename (from, base, NULL);
          g_assert_true (g_file_set_contents (path, base, -1, NULL));
        }
      g_setenv ("BROMELIA_CRASH_DIR", where, TRUE);
      g_setenv ("BROMELIA_CRASH_REPORT", report, TRUE);
      g_setenv ("BROMELIA_CRASH_WHEN", when == 0 ? "before" : "after", TRUE);
      g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
      g_test_trap_assert_failed ();
      if (g_file_get_contents (report, &text, NULL, NULL))
        lines = g_strsplit (text, "\n", -1);
      for (guint i = 0; lines && lines[i] && *lines[i]; i++)
        {
          g_auto (GStrv) pair = g_strsplit (lines[i], "\t", 2);
          g_assert_false (g_file_test (pair[0], G_FILE_TEST_EXISTS));
          g_assert_true (g_file_test (pair[1], G_FILE_TEST_IS_REGULAR));
          reported++;
        }
      for (int i = 0; i < 8; i++)
        {
          g_autofree char *base = g_strdup_printf ("f%d.mkv", i), *path = g_build_filename (from, base, NULL);
          moved += !g_file_test (path, G_FILE_TEST_EXISTS);
        }
      g_assert_cmpuint (reported, ==, when == 0 ? 2 : 3);
      g_assert_cmpuint (moved, ==, 3); /* killed before its report, the third item has moved all the same */
      remove_tree (where);
    }
}

/* A destination folder that can't be written (0555; a run as root, which ignores that, skips): the data disc copy and
 * writeAtomically fail with fs.failed and leave nothing (review follow-ups, read-only paths). */
static void
test_read_only_destination (void)
{
  g_autofree char *dir = g_build_filename (scratch, "read-only", NULL);
  g_autofree char *dest = g_build_filename (dir, "disc.iso", NULL), *record = g_build_filename (dir, "r.json", NULL);
  g_autoptr (TestFakeDriveControl) drives = g_object_new (TEST_TYPE_FAKE_DRIVE_CONTROL, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (TestRecordingSink) sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
  g_autoptr (BroDataImager) imager = NULL;
  g_autoptr (BroBroError) e1 = NULL;
  g_autoptr (BroBroError) e2 = NULL;
  g_autoptr (BroDataImageCopy) copy = NULL;
  g_autoptr (GBytes) bytes = g_bytes_new_static ("{}", 2);
  GDir *d;
  if (geteuid () == 0)
    {
      g_test_skip ("root ignores the folder's permissions");
      return;
    }
  remove_tree (dir);
  g_mkdir_with_parents (dir, 0755);
  g_assert_cmpint (chmod (dir, 0555), ==, 0);
  imager = bro_data_imager_new (BRO_DRIVE_CONTROL (drives), BRO_FILE_SYSTEM (fs));
  drives->disc = g_object_new (TEST_TYPE_FAKE_DISC, NULL);
  drives->disc->sectors = 512;
  copy = bro_data_imager_copy (imager, "/dev/sr0", dest, BRO_RUN_SINK (sink), bro_cancellation_source_token (cancel), &e1);
  g_assert_null (copy);
  g_assert_cmpstr (e1->code, ==, "fs.failed");
  g_assert_false (bro_platform_file_system_write_atomically (fs, record, bytes, 0644, &e2));
  g_assert_cmpstr (e2->code, ==, "fs.failed");
  d = g_dir_open (dir, 0, NULL);
  g_assert_null (g_dir_read_name (d));
  g_dir_close (d);
  chmod (dir, 0755);
  remove_tree (dir);
}
