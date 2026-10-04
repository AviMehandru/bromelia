/* test-adapters.c: the Adapters layer on the real OS. The process cases are those of
 * shared/fixtures/adapters/platform-adapters.contract.json. */
#include "bro-test-english.h"
#include "bro-test-fixtures.h"

#include "bro-fingerprint.h"
#include "bro-home-dir-isolation.h"
#include "bro-makemkv-tool.h"
#include "bro-platform-file-system.h"
#include "bro-platform-process-launcher.h"
#include "bro-platform-tool-paths.h"
#include "bro-apprise-tool.h"
#include "bro-beta-key-page.h"
#include "bro-beta-key-source.h"
#include "bro-episode-details.h"
#include "bro-metadata-client.h"
#include "bro-notification-sender.h"
#include "bro-sqlite-store.h"
#ifdef BRO_HAVE_LIBSOUP
#include "bro-platform-http-client.h"
#endif
#include <gio/gio.h>
#include "bro-system-tool-locator.h"
#include <sqlite3.h>
#include "bro-system-clock.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static char *scratch;

/* ---- processes ------------------------------------------------------------------------------------ */

static BroProcessSpec *
spec_new (const char *exe, ...)
{
  BroProcessSpec *spec = bro_process_spec_new ();
  GPtrArray *args = g_ptr_array_new ();
  va_list ap;
  const char *a;
  spec->executable = g_strdup (exe);
  va_start (ap, exe);
  while ((a = va_arg (ap, const char *)) != NULL)
    g_ptr_array_add (args, g_strdup (a));
  va_end (ap);
  g_ptr_array_add (args, NULL);
  g_strfreev (spec->arguments);
  spec->arguments = (GStrv) g_ptr_array_free (args, FALSE);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  return spec;
}

/* Reads every line, then waits. @lines (optional) gets "stdout:text" / "stderr:text". */
static BroProcessExit
run (BroRunningProcess *p, GPtrArray *lines)
{
  BroOutputLine *line;
  while ((line = bro_running_process_lines (p)) != NULL)
    {
      if (lines)
        g_ptr_array_add (lines, g_strdup_printf ("%s:%s", line->stream == BRO_OUTPUT_SOURCE_STDERR ? "stderr" : "stdout", line->text));
      bro_output_line_free (line);
    }
  return bro_running_process_wait (p);
}

static double
since (gint64 start)
{
  return (double) (g_get_monotonic_time () - start) / G_USEC_PER_SEC;
}

static BroRunningProcess *
start (BroProcessSpec *spec)
{
  g_autoptr (BroPlatformProcessLauncher) launcher = bro_platform_process_launcher_new ();
  g_autoptr (BroBroError) error = NULL;
  BroRunningProcess *p = bro_process_launcher_start (BRO_PROCESS_LAUNCHER (launcher), spec, &error);
  g_assert_null (error);
  g_assert_nonnull (p);
  return p;
}

static void
test_stall_stops (void)
{
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", "echo started; sleep 60", NULL);
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  gint64 t0 = g_get_monotonic_time ();
  g_autoptr (BroRunningProcess) p = NULL;
  BroProcessExit exit;
  spec->stop_policy = BRO_STOP_POLICY_TERMINATE_FIRST;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = 2;
  p = start (spec);
  exit = run (p, lines);
  g_assert_cmpfloat (exit.stalled_seconds, >=, 2);
  g_assert_cmpint (exit.status, !=, 0);
  g_assert_false (exit.cancelled);
  g_assert_cmpfloat (since (t0), <, 15);
  g_assert_cmpuint (lines->len, ==, 1);
  g_assert_cmpstr (lines->pdata[0], ==, "stdout:started");
}

static void
test_busy_not_stopped (void)
{
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", "for i in 1 2 3 4 5 6; do echo $i; sleep 0.5; done", NULL);
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroRunningProcess) p = NULL;
  BroProcessExit exit;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = 2;
  p = start (spec);
  exit = run (p, lines);
  g_assert_cmpfloat (exit.stalled_seconds, <, 0);
  g_assert_cmpint (exit.status, ==, 0);
  g_assert_cmpuint (lines->len, ==, 6);
  g_assert_cmpstr (lines->pdata[5], ==, "stdout:6");
}

/* cancel-escalates: INT is ignored, TERM 5 s later ends it. Works without any main loop running. */
static void
test_cancel_escalates (void)
{
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", "trap '' INT; echo ready; sleep 60 & wait", NULL);
  gint64 t0 = g_get_monotonic_time ();
  g_autoptr (BroRunningProcess) p = start (spec);
  BroProcessExit exit;
  g_usleep (G_USEC_PER_SEC / 2);
  bro_running_process_stop (p, BRO_STOP_REASON_CANCELLED);
  exit = run (p, NULL);
  g_assert_true (exit.cancelled);
  g_assert_cmpint (exit.signal, ==, SIGTERM);
  g_assert_cmpfloat (since (t0), >=, 5);
  g_assert_cmpfloat (since (t0), <, 12);
}

/* terminate-first, and KILL 5 s after a TERM that is ignored too. */
static void
test_terminate_first (void)
{
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", "echo ready; sleep 60", NULL);
  g_autoptr (BroProcessSpec) stubborn = spec_new ("/bin/sh", "-c", "trap '' INT TERM; echo ready; while :; do sleep 0.1; done", NULL);
  g_autoptr (BroRunningProcess) p = NULL;
  g_autoptr (BroRunningProcess) q = NULL;
  BroProcessExit exit;
  gint64 t0;
  spec->stop_policy = stubborn->stop_policy = BRO_STOP_POLICY_TERMINATE_FIRST;
  p = start (spec);
  g_usleep (G_USEC_PER_SEC * 3 / 10);
  bro_running_process_stop (p, BRO_STOP_REASON_SHUTDOWN);
  exit = run (p, NULL);
  g_assert_cmpint (exit.signal, ==, SIGTERM);
  g_assert_true (exit.cancelled);

  t0 = g_get_monotonic_time ();
  q = start (stubborn);
  g_usleep (G_USEC_PER_SEC * 3 / 10);
  bro_running_process_stop (q, BRO_STOP_REASON_CANCELLED);
  exit = run (q, NULL);
  g_assert_cmpint (exit.signal, ==, SIGKILL);
  g_assert_cmpint (exit.status, ==, -1);
  g_assert_cmpfloat (since (t0), >=, 5);
  g_assert_cmpfloat (since (t0), <, 12);
}

static void
test_output_drained (void)
{
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", "sleep 20 & echo done", NULL);
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  gint64 t0 = g_get_monotonic_time ();
  g_autoptr (BroRunningProcess) p = start (spec);
  BroProcessExit exit = run (p, lines);
  g_assert_cmpint (exit.status, ==, 0);
  g_assert_cmpuint (lines->len, ==, 1);
  g_assert_cmpstr (lines->pdata[0], ==, "stdout:done");
  g_assert_cmpfloat (since (t0), <, 8);
}

static void
test_process_group (void)
{
  g_autofree char *pid_file = g_build_filename (scratch, "child.pid", NULL);
  g_autofree char *script = g_strdup_printf ("sleep 60 & echo $! > '%s'; sleep 60", pid_file);
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", script, NULL);
  g_autoptr (BroRunningProcess) p = start (spec);
  g_autofree char *text = NULL;
  pid_t child;
  for (int i = 0; i < 100; i++)
    {
      g_clear_pointer (&text, g_free);
      if (g_file_get_contents (pid_file, &text, NULL, NULL) && *text)
        break;
      g_usleep (G_USEC_PER_SEC / 20);
    }
  child = (pid_t) atoi (text);
  g_assert_cmpint (child, >, 0);
  bro_running_process_stop (p, BRO_STOP_REASON_CANCELLED);
  run (p, NULL);
  g_usleep (G_USEC_PER_SEC * 3 / 10);
  g_assert_cmpint (kill (child, 0), !=, 0);
}

static char *
replace_instants (const char *text)
{
  g_autoptr (GRegex) re = g_regex_new ("\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z", 0, 0, NULL);
  return g_regex_replace_literal (re, text, -1, 0, "<instant>", 0, NULL);
}

static void
test_transcript (void)
{
  g_autofree char *path = g_build_filename (scratch, "logs", "t.txt", NULL);
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/echo", "hi", NULL);
  g_autoptr (BroRunningProcess) p = NULL;
  g_autofree char *text = NULL;
  g_autofree char *shown = NULL;
  spec->transcript = g_strdup (path);
  p = start (spec);
  run (p, NULL);
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  shown = replace_instants (text);
  g_assert_cmpstr (shown, ==, "==== <instant> $ /bin/echo hi\nhi\n==== <instant> exit status 0\n");
}

static void
test_streams (void)
{
  g_autoptr (BroProcessSpec) spec = spec_new ("/bin/sh", "-c", "echo $BRO_TEST_VALUE; pwd; echo oops >&2", NULL);
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroRunningProcess) p = NULL;
  g_autofree char *pwd = NULL;
  g_autofree char *pwd_link = g_strdup_printf ("stdout:%s", scratch);
  char *real = realpath (scratch, NULL);
  BroProcessExit exit;
  g_hash_table_insert (spec->environment, g_strdup ("BRO_TEST_VALUE"), g_strdup ("from the spec"));
  spec->working_directory = g_strdup (scratch);
  p = start (spec);
  exit = run (p, lines);
  g_assert_cmpint (exit.status, ==, 0);
  pwd = g_strdup_printf ("stdout:%s", real);
  free (real);
  g_assert_true (g_ptr_array_find_with_equal_func (lines, "stdout:from the spec", g_str_equal, NULL));
  g_assert_true (g_ptr_array_find_with_equal_func (lines, pwd, g_str_equal, NULL)
                 || g_ptr_array_find_with_equal_func (lines, pwd_link, g_str_equal, NULL));
  g_assert_true (g_ptr_array_find_with_equal_func (lines, "stderr:oops", g_str_equal, NULL));
}

static void
test_could_not_start (void)
{
  g_autoptr (BroPlatformProcessLauncher) launcher = bro_platform_process_launcher_new ();
  g_autofree char *missing = g_build_filename (scratch, "missing", NULL);
  g_autofree char *nope = g_build_filename (scratch, "nope", NULL);
  g_autoptr (BroProcessSpec) a = spec_new (missing, NULL);
  g_autoptr (BroProcessSpec) b = spec_new ("/bin/echo", NULL);
  g_autoptr (BroBroError) e1 = NULL;
  g_autoptr (BroBroError) e2 = NULL;
  g_assert_null (bro_process_launcher_start (BRO_PROCESS_LAUNCHER (launcher), a, &e1));
  g_assert_cmpstr (e1->code, ==, "process.couldNotStart");
  b->working_directory = g_strdup (nope);
  g_assert_null (bro_process_launcher_start (BRO_PROCESS_LAUNCHER (launcher), b, &e2));
  g_assert_cmpstr (e2->code, ==, "process.couldNotStart");
}

static void
test_interpreter (void)
{
  g_autofree char *script = g_build_filename (scratch, "hello.sh", NULL);
  g_autoptr (BroProcessSpec) plain = spec_new (script, "x", NULL);
  g_autoptr (BroProcessSpec) custom = spec_new (script, "x", NULL);
  g_autoptr (BroProcessSpec) echo = spec_new ("/bin/echo", "x", NULL);
  g_autoptr (BroProcessSpec) there = spec_new (script, "there", NULL);
  g_autoptr (BroCommandLine) c1 = NULL;
  g_autoptr (BroCommandLine) c2 = NULL;
  g_autoptr (BroCommandLine) c3 = NULL;
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroRunningProcess) p = NULL;
  BroProcessExit exit;
  g_assert_true (g_file_set_contents (script, "echo hello $1\n", -1, NULL));
  c1 = bro_platform_process_launcher_invocation (plain);
  g_assert_cmpstr (c1->executable, ==, "/bin/sh");
  g_assert_cmpstr (c1->arguments[0], ==, script);
  g_assert_cmpstr (c1->arguments[1], ==, "x");
  custom->interpreter = g_strdup ("/usr/bin/env");
  c2 = bro_platform_process_launcher_invocation (custom);
  g_assert_cmpstr (c2->executable, ==, "/usr/bin/env");
  g_assert_cmpstr (c2->arguments[0], ==, script);
  c3 = bro_platform_process_launcher_invocation (echo);
  g_assert_cmpstr (c3->executable, ==, "/bin/echo");
  g_assert_cmpstr (c3->arguments[0], ==, "x");
  g_assert_null (c3->arguments[1]);
  p = start (there);
  exit = run (p, lines);
  g_assert_cmpint (exit.status, ==, 0);
  g_assert_cmpuint (lines->len, ==, 1);
  g_assert_cmpstr (lines->pdata[0], ==, "stdout:hello there");
}

/* ---- the file system ------------------------------------------------------------------------------- */

static char *fs_root;

static char *
fs_path (const char *relative)
{
  return g_str_equal (relative, ".") ? g_strdup (fs_root) : g_build_filename (fs_root, relative, NULL);
}

static void
remove_tree (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name;
  if (dir)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          remove_tree (child);
        }
      g_dir_close (dir);
      g_rmdir (path);
    }
  else
    g_unlink (path);
}

static void
fs_build (BroJsonValue *tree)
{
  remove_tree (fs_root);
  g_mkdir_with_parents (fs_root, 0755);
  for (guint i = 0; i < bro_json_value_length (tree); i++)
    {
      const char *name = tree->keys->pdata[i];
      g_autofree char *path = NULL;
      if (g_str_has_suffix (name, "/"))
        {
          g_autofree char *trimmed = g_strndup (name, strlen (name) - 1);
          path = fs_path (trimmed);
          g_mkdir_with_parents (path, 0755);
        }
      else
        {
          path = fs_path (name);
          g_file_set_contents (path, bro_json_value_get_string (bro_json_value_at (tree, i), ""), -1, NULL);
        }
    }
}

static void
fs_walk (const char *dir, const char *prefix, GPtrArray *lines)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  if (!d)
    return;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      g_autofree char *full = g_build_filename (dir, name, NULL);
      g_autofree char *rel = *prefix ? g_strdup_printf ("%s/%s", prefix, name) : g_strdup (name);
      if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
          g_ptr_array_add (lines, g_strdup_printf ("%s/ = (folder)", rel));
          fs_walk (full, rel, lines);
        }
      else
        {
          g_autofree char *text = NULL;
          g_file_get_contents (full, &text, NULL, NULL);
          g_ptr_array_add (lines, g_strdup_printf ("%s = %s", rel, text ? text : "?"));
        }
    }
  g_dir_close (d);
}

/* Sorts by the name before " = ". */
static int
compare_tree_lines (gconstpointer a, gconstpointer b)
{
  const char *x = *(const char *const *) a, *y = *(const char *const *) b;
  size_t lx = strstr (x, " = ") - x, ly = strstr (y, " = ") - y;
  int c = memcmp (x, y, MIN (lx, ly));
  return c ? c : (lx < ly ? -1 : lx > ly);
}

static char *
fs_tree (void)
{
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  fs_walk (fs_root, "", lines);
  g_ptr_array_sort (lines, compare_tree_lines);
  g_ptr_array_add (lines, NULL);
  return g_strjoinv ("\n", (char **) lines->pdata);
}

static char *
fs_expected_tree (BroJsonValue *tree)
{
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < bro_json_value_length (tree); i++)
    {
      BroJsonValue *v = bro_json_value_at (tree, i);
      g_ptr_array_add (lines, g_strdup_printf ("%s = %s", (char *) tree->keys->pdata[i], bro_json_value_get_string (v, "(folder)")));
    }
  g_ptr_array_sort (lines, compare_tree_lines);
  g_ptr_array_add (lines, NULL);
  return g_strjoinv ("\n", (char **) lines->pdata);
}

static BroJsonValue *
string_of_bytes (GBytes *bytes)
{
  gsize n = 0;
  const char *data = g_bytes_get_data (bytes, &n);
  g_autofree char *text = g_strndup (data ? data : "", n);
  g_bytes_unref (bytes);
  return bro_json_value_new_string (text);
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

/* Runs one operation: its result (NULL when it has none), or NULL with @error set. */
static BroJsonValue *
fs_run (BroFileSystem *fs, BroJsonValue *op, BroBroError **error, gboolean *ok)
{
  const char *name = bro_json_value_get_string (bro_json_value_at (op, 0), "");
  g_autofree char *a = bro_json_value_length (op) > 1 && bro_json_value_at (op, 1)->kind == BRO_JSON_VALUE_STRING
                         ? fs_path (bro_json_value_get_string (bro_json_value_at (op, 1), "")) : NULL;
  g_autofree char *b = bro_json_value_length (op) > 2 && bro_json_value_at (op, 2)->kind == BRO_JSON_VALUE_STRING
                         ? fs_path (bro_json_value_get_string (bro_json_value_at (op, 2), "")) : NULL;
  *ok = TRUE;
  if (g_str_equal (name, "exists"))
    return bro_json_value_new_bool (bro_file_system_exists (fs, a));
  if (g_str_equal (name, "stat"))
    {
      g_autoptr (BroFileInfo) info = bro_file_system_stat (fs, a, error);
      BroJsonValue *v;
      if (!info)
        return *ok = FALSE, NULL;
      v = bro_json_value_new_object ();
      if (!info->is_directory)
        bro_json_value_set (v, "size", bro_json_value_new_integer (info->size));
      bro_json_value_set (v, "isDirectory", bro_json_value_new_bool (info->is_directory));
      return v;
    }
  if (g_str_equal (name, "list"))
    {
      g_autoptr (GPtrArray) entries = bro_file_system_list (fs, a, error);
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      BroJsonValue *v;
      if (!entries)
        return *ok = FALSE, NULL;
      for (guint i = 0; i < entries->len; i++)
        {
          BroDirectoryEntry *e = entries->pdata[i];
          g_ptr_array_add (names, g_strconcat (e->name, e->is_directory ? "/" : "", NULL));
        }
      g_ptr_array_sort (names, compare_strings);
      v = bro_json_value_new_array ();
      for (guint i = 0; i < names->len; i++)
        bro_json_value_append (v, bro_json_value_new_string (names->pdata[i]));
      return v;
    }
  if (g_str_equal (name, "read"))
    {
      GBytes *bytes = bro_file_system_read (fs, a, error);
      return bytes ? string_of_bytes (bytes) : (*ok = FALSE, NULL);
    }
  if (g_str_equal (name, "readRange"))
    {
      GBytes *bytes = bro_file_system_read_range (fs, a, bro_json_value_get_integer (bro_json_value_at (op, 2), 0),
                                                  (int) bro_json_value_get_integer (bro_json_value_at (op, 3), 0), error);
      return bytes ? string_of_bytes (bytes) : (*ok = FALSE, NULL);
    }
  if (g_str_equal (name, "createDirectory"))
    *ok = bro_file_system_create_directory (fs, a, bro_json_value_get_bool (bro_json_value_at (op, 2), FALSE), error);
  else if (g_str_equal (name, "writeAtomically"))
    {
      const char *text = bro_json_value_get_string (bro_json_value_at (op, 2), "");
      g_autoptr (GBytes) bytes = g_bytes_new (text, strlen (text));
      *ok = bro_file_system_write_atomically (fs, a, bytes, (int) bro_json_value_get_integer (bro_json_value_at (op, 3), 0644), error);
    }
  else if (g_str_equal (name, "rename"))
    *ok = bro_file_system_rename (fs, a, b, error);
  else if (g_str_equal (name, "moveMerging"))
    {
      BroMovePolicy policy = BRO_MOVE_POLICY_NEVER_REPLACE;
      g_autoptr (GPtrArray) moved = NULL;
      BroJsonValue *v;
      bro_move_policy_from_wire (bro_json_value_get_string (bro_json_value_at (op, 3), ""), &policy);
      moved = bro_file_system_move_merging (fs, a, b, policy, error);
      if (!moved)
        return *ok = FALSE, NULL;
      v = bro_json_value_new_array ();
      for (guint i = 0; i < moved->len; i++)
        {
          BroMovedItem *m = moved->pdata[i];
          BroJsonValue *pair = bro_json_value_new_array ();
          bro_json_value_append (pair, bro_json_value_new_string (m->from + strlen (fs_root) + 1));
          bro_json_value_append (pair, bro_json_value_new_string (m->to + strlen (fs_root) + 1));
          bro_json_value_append (v, pair);
        }
      return v;
    }
  else if (g_str_equal (name, "remove"))
    *ok = bro_file_system_remove (fs, a, error);
  else if (g_str_equal (name, "moveToTrash"))
    *ok = bro_file_system_move_to_trash (fs, a, b, error);
  else if (g_str_equal (name, "syncFile"))
    *ok = bro_file_system_sync_file (fs, a, error);
  else if (g_str_equal (name, "syncDirectory"))
    *ok = bro_file_system_sync_directory (fs, a, error);
  else if (g_str_equal (name, "openForReading"))
    {
      g_autoptr (BroByteStream) stream = bro_file_system_open_for_reading (fs, a, bro_json_value_get_bool (bro_json_value_at (op, 2), FALSE), error);
      GByteArray *all;
      GBytes *chunk;
      if (!stream)
        return *ok = FALSE, NULL;
      all = g_byte_array_new ();
      while ((chunk = bro_byte_stream_read (stream, 4, NULL)) != NULL && g_bytes_get_size (chunk) > 0)
        {
          gsize n;
          const guint8 *d = g_bytes_get_data (chunk, &n);
          g_byte_array_append (all, d, (guint) n);
          g_bytes_unref (chunk);
        }
      if (chunk)
        g_bytes_unref (chunk);
      bro_byte_stream_close (stream);
      return string_of_bytes (g_byte_array_free_to_bytes (all));
    }
  return NULL;
}

static gboolean
fs_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroJsonValue) result = NULL;
  BroJsonValue *want_error = bro_json_value_member (expect, "error");
  BroJsonValue *want_result = bro_json_value_member (expect, "result");
  BroJsonValue *want_tree = bro_json_value_member (expect, "tree");
  gboolean ok;
  fs_build (bro_json_value_member (given, "tree"));
  result = fs_run (BRO_FILE_SYSTEM (fs), bro_json_value_member (given, "op"), &error, &ok);
  if (want_error)
    {
      BroJsonValue *params = bro_json_value_member (want_error, "params");
      if (ok || !error)
        {
          bro_test_fail (failures, id, "no error");
          return TRUE;
        }
      bro_test_same_string (failures, id, "code", bro_json_value_get_string (bro_json_value_member (want_error, "code"), ""), error->code);
      for (guint i = 0; params && i < bro_json_value_length (params); i++)
        {
          const char *key = params->keys->pdata[i];
          const char *actual = bro_json_value_get_string (bro_json_value_member (error->params, key), "");
          g_autofree char *shown = g_str_equal (key, "path") && g_str_has_prefix (actual, fs_root)
                                     ? g_strconcat ("<root>", actual + strlen (fs_root), NULL) : g_strdup (actual);
          bro_test_same_string (failures, id, key, bro_json_value_get_string (bro_json_value_at (params, i), ""), shown);
        }
    }
  else if (!ok)
    bro_test_fail (failures, id, "failed: %s", error ? error->code : "?");
  else if (want_result && want_result->kind == BRO_JSON_VALUE_OBJECT && result && result->kind == BRO_JSON_VALUE_OBJECT)
    {
      for (guint i = 0; i < bro_json_value_length (want_result); i++)
        bro_test_same_json (failures, id, want_result->keys->pdata[i], bro_json_value_at (want_result, i),
                            bro_json_value_member (result, want_result->keys->pdata[i]));
    }
  else if (want_result)
    bro_test_same_json (failures, id, "result", want_result, result);
  if (want_tree)
    {
      g_autofree char *want = fs_expected_tree (want_tree);
      g_autofree char *got = fs_tree ();
      bro_test_same_string (failures, id, "tree", want, got);
    }
  return TRUE;
}

static void
test_fs_cases (void)
{
  bro_test_run_cases ("adapters/file-system.cases.json", fs_case);
}

/* file-system-durable-write, file-system-read-uncached, and the permission bits. */
static void
test_fs_durable_and_uncached (void)
{
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree guint8 *big = g_malloc ((3 << 20) + 123);
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (BroByteStream) stream = NULL;
  g_autoptr (GByteArray) all = g_byte_array_new ();
  GBytes *chunk;
  struct stat st;
  GDir *dir;
  for (gsize i = 0; i < (3 << 20) + 123; i++)
    big[i] = (guint8) ((i * 2654435761u) >> 13);
  remove_tree (fs_root);
  g_mkdir_with_parents (fs_root, 0755);
  path = g_build_filename (fs_root, "big.bin", NULL);
  bytes = g_bytes_new_static (big, (3 << 20) + 123);
  g_assert_true (bro_platform_file_system_write_atomically (fs, path, bytes, 0600, &error));
  g_assert_true (bro_platform_file_system_sync_file (fs, path, &error));
  g_assert_true (bro_platform_file_system_sync_directory (fs, fs_root, &error));
  g_assert_cmpint (stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  stream = bro_platform_file_system_open_for_reading (fs, path, TRUE, &error);
  g_assert_nonnull (stream);
  while ((chunk = bro_byte_stream_read (stream, 100000, &error)) != NULL && g_bytes_get_size (chunk) > 0)
    {
      gsize n;
      const guint8 *d = g_bytes_get_data (chunk, &n);
      g_byte_array_append (all, d, (guint) n);
      g_bytes_unref (chunk);
    }
  g_clear_pointer (&chunk, g_bytes_unref);
  bro_byte_stream_close (stream);
  g_assert_cmpuint (all->len, ==, (3 << 20) + 123);
  g_assert_true (memcmp (all->data, big, all->len) == 0);
  dir = g_dir_open (fs_root, 0, NULL);
  g_assert_cmpstr (g_dir_read_name (dir), ==, "big.bin");
  g_assert_null (g_dir_read_name (dir));
  g_dir_close (dir);
}

static void
test_fs_volume (void)
{
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *missing = g_build_filename (fs_root, "not", "yet", "there", NULL);
  g_autoptr (BroVolumeInfo) v = NULL;
  g_autoptr (BroVolumeInfo) w = NULL;
  g_mkdir_with_parents (fs_root, 0755);
  v = bro_platform_file_system_volume (fs, missing, &error);
  w = bro_platform_file_system_volume (fs, fs_root, &error);
  g_assert_nonnull (v);
  g_assert_cmpuint (strlen (v->id), ==, 16);
  g_assert_cmpint (v->free_bytes, >, 0);
  g_assert_cmpstr (v->fs_type, !=, "");
  g_assert_cmpstr (v->id, ==, w->id);
#ifdef __APPLE__
  g_assert_cmpstr (v->fs_type, ==, "apfs");
#else
  g_assert_cmpstr (v->fs_type, !=, "unknown");
#endif
}

/* ---- settings isolation (HOME) --------------------------------------------------------------------- */

static char *
shown_path (const char *path)
{
  return path ? g_strconcat ("<root>", path + strlen (fs_root), NULL) : g_strdup ("(none)");
}

static void
list_files (const char *dir, GPtrArray *out)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  if (!d)
    return;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      g_autofree char *full = g_build_filename (dir, name, NULL);
      if (g_file_test (full, G_FILE_TEST_IS_DIR))
        list_files (full, out);
      else
        g_ptr_array_add (out, g_strdup (full + strlen (fs_root) + 1));
    }
  g_dir_close (d);
}

static gboolean
isolation_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroHomeDirIsolation) iso = NULL;
  g_autoptr (BroMakemkvRunSettings) run = bro_makemkv_run_settings_new ();
  g_autoptr (BroIsolationLease) lease = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GHashTable) env = NULL;
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *profile = NULL;
  g_autofree char *shown = NULL;
  BroJsonValue *before = bro_json_value_member (given, "before");
  BroJsonValue *settings = bro_json_value_member (given, "settings");
  BroJsonValue *want_env = bro_json_value_member (expect, "environment");
  BroJsonValue *want_files = bro_json_value_member (expect, "files");
  BroJsonValue *modes = bro_json_value_member (expect, "modes");
  BroJsonValue *xml = bro_json_value_member (given, "profileXml");
  BroHomeLayout layout = BRO_HOME_LAYOUT_LINUX;
  remove_tree (fs_root);
  g_mkdir_with_parents (fs_root, 0755);
  for (guint i = 0; before && i < bro_json_value_length (before); i++)
    {
      g_autofree char *path = fs_path (before->keys->pdata[i]);
      g_autofree char *dir = g_path_get_dirname (path);
      g_mkdir_with_parents (dir, 0755);
      g_file_set_contents (path, bro_json_value_get_string (bro_json_value_at (before, i), ""), -1, NULL);
    }
  bro_home_layout_from_wire (bro_json_value_get_string (bro_json_value_member (given, "layout"), ""), &layout);
  for (guint i = 0; i < bro_json_value_length (settings); i++)
    g_hash_table_insert (run->settings, g_strdup (settings->keys->pdata[i]), g_strdup (bro_json_value_get_string (bro_json_value_at (settings, i), "")));
  run->profile_xml = g_strdup (bro_json_value_get_string (xml, NULL));
  run->data_dir = g_strdup ("/unused");
  run->work_directory = fs_path (bro_json_value_get_string (bro_json_value_member (given, "workDirectory"), ""));
  iso = bro_home_dir_isolation_new (BRO_FILE_SYSTEM (fs), layout);
  lease = bro_settings_isolation_prepare (BRO_SETTINGS_ISOLATION (iso), run, &error);
  if (!lease)
    {
      bro_test_fail (failures, id, "prepare failed: %s", error ? error->code : "?");
      return TRUE;
    }
  bro_isolation_lease_first_output (lease);
  bro_isolation_lease_release (lease);
  env = bro_isolation_lease_environment (lease);
  for (guint i = 0; i < bro_json_value_length (want_env); i++)
    {
      g_autofree char *got = shown_path (g_hash_table_lookup (env, want_env->keys->pdata[i]));
      bro_test_same_string (failures, id, want_env->keys->pdata[i], bro_json_value_get_string (bro_json_value_at (want_env, i), ""), got);
    }
  profile = bro_isolation_lease_profile_path (lease);
  shown = shown_path (profile);
  bro_test_same_string (failures, id, "profilePath", bro_json_value_get_string (bro_json_value_member (expect, "profilePath"), "(none)"), shown);
  list_files (run->work_directory, files);
  g_ptr_array_sort (files, compare_strings);
  if (files->len != bro_json_value_length (want_files))
    bro_test_fail (failures, id, "%u files, expected %u", files->len, bro_json_value_length (want_files));
  for (guint i = 0; i < bro_json_value_length (want_files); i++)
    {
      g_autofree char *path = fs_path (want_files->keys->pdata[i]);
      g_autofree char *text = NULL;
      g_file_get_contents (path, &text, NULL, NULL);
      bro_test_same_string (failures, id, want_files->keys->pdata[i], bro_json_value_get_string (bro_json_value_at (want_files, i), ""), text);
    }
  for (guint i = 0; modes && i < bro_json_value_length (modes); i++)
    {
      g_autofree char *path = fs_path (modes->keys->pdata[i]);
      struct stat st;
      if (stat (path, &st) != 0 || (gint64) (st.st_mode & 0777) != bro_json_value_get_integer (bro_json_value_at (modes, i), -1))
        bro_test_fail (failures, id, "mode of %s", (char *) modes->keys->pdata[i]);
    }
  return TRUE;
}

static void
test_home_isolation (void)
{
  bro_test_run_cases ("adapters/settings-isolation.cases.json", isolation_case);
}

/* ---- the tool locator ------------------------------------------------------------------------------ */

static char *
with_root (const char *text)
{
  g_autoptr (GString) s = g_string_new (text);
  g_string_replace (s, "<root>", fs_root, 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static GHashTable *
tool_lists (BroJsonValue *v, gboolean rooted)
{
  GHashTable *t = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_strfreev);
  for (guint i = 0; v && i < bro_json_value_length (v); i++)
    {
      BroJsonValue *list = bro_json_value_at (v, i);
      BroToolKind tool;
      char **items = g_new0 (char *, bro_json_value_length (list) + 1);
      for (guint j = 0; j < bro_json_value_length (list); j++)
        {
          const char *x = bro_json_value_get_string (bro_json_value_at (list, j), "");
          items[j] = rooted ? with_root (x) : g_strdup (x);
        }
      if (bro_tool_kind_from_wire (v->keys->pdata[i], &tool))
        g_hash_table_insert (t, GINT_TO_POINTER (tool), items);
      else
        g_strfreev (items);
    }
  return t;
}

static gboolean
locator_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (GHashTable) configured = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  g_autoptr (GHashTable) candidates = tool_lists (bro_json_value_member (given, "candidates"), TRUE);
  g_autoptr (GHashTable) names = tool_lists (bro_json_value_member (given, "names"), FALSE);
  g_autoptr (GPtrArray) search = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroSystemToolLocator) locator = NULL;
  g_autoptr (BroToolInfo) info = NULL;
  g_autofree char *home = with_root (bro_json_value_get_string (bro_json_value_member (given, "home"), ""));
  BroJsonValue *files = bro_json_value_member (given, "files");
  BroJsonValue *conf = bro_json_value_member (given, "configured");
  BroJsonValue *path_list = bro_json_value_member (given, "searchPath");
  const char *want = bro_json_value_get_string (bro_json_value_member (expect, "path"), NULL);
  BroToolKind tool = BRO_TOOL_KIND_MAKEMKVCON;
  remove_tree (fs_root);
  g_mkdir_with_parents (fs_root, 0755);
  for (guint i = 0; i < bro_json_value_length (files); i++)
    {
      const char *f = bro_json_value_get_string (bro_json_value_at (files, i), "");
      g_autofree char *path = fs_path (f);
      if (g_str_has_suffix (f, "/"))
        g_mkdir_with_parents (path, 0755);
      else
        {
          g_autofree char *dir = g_path_get_dirname (path);
          g_mkdir_with_parents (dir, 0755);
          g_file_set_contents (path, "", 0, NULL);
        }
    }
  for (guint i = 0; i < bro_json_value_length (conf); i++)
    if (bro_tool_kind_from_wire (conf->keys->pdata[i], &tool))
      g_hash_table_insert (configured, GINT_TO_POINTER (tool), with_root (bro_json_value_get_string (bro_json_value_at (conf, i), "")));
  for (guint i = 0; i < bro_json_value_length (path_list); i++)
    g_ptr_array_add (search, with_root (bro_json_value_get_string (bro_json_value_at (path_list, i), "")));
  g_ptr_array_add (search, NULL);
  locator = bro_system_tool_locator_new (BRO_FILE_SYSTEM (fs), configured, candidates, names, (const char *const *) search->pdata, home);
  bro_tool_kind_from_wire (bro_json_value_get_string (bro_json_value_member (given, "tool"), ""), &tool);
  info = bro_tool_locator_locate (BRO_TOOL_LOCATOR (locator), tool);
  if (want)
    {
      g_autofree char *got = shown_path (info->path);
      bro_test_same_string (failures, id, "path", want, got);
      if (info->why)
        bro_test_fail (failures, id, "a reason although found");
    }
  else
    {
      BroJsonValue *why = bro_json_value_member (expect, "why");
      BroJsonValue *params = bro_json_value_member (why, "params");
      g_autoptr (BroJsonValue) got = info->why ? bro_bro_message_to_json (info->why) : NULL;
      BroJsonValue *got_params = got ? bro_json_value_member (got, "params") : NULL;
      if (info->path)
        bro_test_fail (failures, id, "found %s", info->path);
      bro_test_same_string (failures, id, "code", bro_json_value_get_string (bro_json_value_member (why, "code"), ""),
                            got ? bro_json_value_get_string (bro_json_value_member (got, "code"), "") : NULL);
      for (guint i = 0; params && i < bro_json_value_length (params); i++)
        {
          const char *key = params->keys->pdata[i];
          const char *actual = bro_json_value_get_string (bro_json_value_member (got_params, key), "");
          g_autofree char *shown = g_str_equal (key, "path") ? shown_path (actual) : g_strdup (actual);
          bro_test_same_string (failures, id, key, bro_json_value_get_string (bro_json_value_at (params, i), ""), shown);
        }
    }
  return TRUE;
}

static void
test_tool_locator (void)
{
  bro_test_run_cases ("adapters/tool-locator.cases.json", locator_case);
}

static void
test_tool_paths (void)
{
  g_autoptr (GHashTable) c = bro_platform_tool_paths_candidates ("/home/me");
  g_autoptr (GHashTable) n = bro_platform_tool_paths_names ();
  const char *const *mk = g_hash_table_lookup (c, GINT_TO_POINTER (BRO_TOOL_KIND_MAKEMKVCON));
  const char *const *hb = g_hash_table_lookup (n, GINT_TO_POINTER (BRO_TOOL_KIND_HANDBRAKE));
  g_assert_true (g_strv_contains (mk, "/usr/bin/makemkvcon"));
  g_assert_cmpstr (hb[0], ==, "HandBrakeCLI");
  g_assert_cmpuint (g_hash_table_size (n), ==, BRO_TOOL_KIND_APPRISE + 1);
}

/* ---- the makemkvcon tool, against a scripted launcher ---------------------------------------------- */

/* A process that plays a script: the lines, one per call to lines(); then the exit status, after creating a file in
 * the destination. A stop ends it at once with status -1 and signal 15. */
#define TEST_TYPE_SCRIPT (test_script_get_type ())
G_DECLARE_FINAL_TYPE (TestScript, test_script, TEST, SCRIPT, GObject)

struct _TestScript {
  GObject parent_instance;
  GStrv lines;
  int exit_code;
  char *write_file_in;
  int cancel_after;
  BroCancellationToken *cancel;
  int handed;
  gboolean stopped;
  BroStopReason reason;
  BroProcessExit exit;
};

static void test_script_iface_init (BroRunningProcessInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestScript, test_script, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_RUNNING_PROCESS, test_script_iface_init))

static BroOutputLine *
script_lines (BroRunningProcess *p)
{
  TestScript *self = TEST_SCRIPT (p);
  BroOutputLine *line;
  if (!self->stopped && self->handed > 0 && self->handed == self->cancel_after && self->cancel)
    bro_cancellation_token_cancel (self->cancel);
  if (!self->stopped && self->lines && self->lines[self->handed])
    {
      line = bro_output_line_new ();
      line->stream = BRO_OUTPUT_SOURCE_STDOUT;
      line->text = g_strdup (self->lines[self->handed++]);
      return line;
    }
  if (self->stopped)
    self->exit = (BroProcessExit) { -1, 15, -1, FALSE, self->reason == BRO_STOP_REASON_CANCELLED || self->reason == BRO_STOP_REASON_SHUTDOWN };
  else
    {
      if (self->write_file_in)
        {
          g_autofree char *f = g_build_filename (self->write_file_in, "title_t00.mkv", NULL);
          g_file_set_contents (f, "", 0, NULL);
        }
      self->exit = bro_process_exit_status (self->exit_code);
    }
  return NULL;
}

static BroProcessExit
script_wait (BroRunningProcess *p)
{
  return TEST_SCRIPT (p)->exit;
}

static void
script_stop (BroRunningProcess *p, BroStopReason reason)
{
  TestScript *self = TEST_SCRIPT (p);
  if (!self->stopped)
    {
      self->stopped = TRUE;
      self->reason = reason;
    }
}

static void
test_script_finalize (GObject *o)
{
  TestScript *self = TEST_SCRIPT (o);
  g_strfreev (self->lines);
  g_free (self->write_file_in);
  G_OBJECT_CLASS (test_script_parent_class)->finalize (o);
}

static void test_script_class_init (TestScriptClass *k) { G_OBJECT_CLASS (k)->finalize = test_script_finalize; }
static void test_script_init (TestScript *self) { self->cancel_after = -1; }

static void
test_script_iface_init (BroRunningProcessInterface *iface)
{
  iface->lines = script_lines;
  iface->wait = script_wait;
  iface->stop = script_stop;
}

#define TEST_TYPE_SCRIPTED_LAUNCHER (test_scripted_launcher_get_type ())
G_DECLARE_FINAL_TYPE (TestScriptedLauncher, test_scripted_launcher, TEST, SCRIPTED_LAUNCHER, GObject)

struct _TestScriptedLauncher {
  GObject parent_instance;
  GStrv lines;
  int exit_code;
  char *write_file_in;
  int cancel_after;
  BroCancellationToken *cancel;
  GPtrArray *started; /* BroProcessSpec * (copies of what matters) */
  TestScript *last;
};

static void test_scripted_launcher_iface_init (BroProcessLauncherInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestScriptedLauncher, test_scripted_launcher, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_PROCESS_LAUNCHER, test_scripted_launcher_iface_init))

static BroProcessSpec *
spec_copy (const BroProcessSpec *spec)
{
  BroProcessSpec *c = bro_process_spec_new ();
  GHashTableIter it;
  gpointer k, v;
  c->executable = g_strdup (spec->executable);
  g_strfreev (c->arguments);
  c->arguments = g_strdupv (spec->arguments);
  g_hash_table_iter_init (&it, spec->environment);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_insert (c->environment, g_strdup (k), g_strdup (v));
  c->working_directory = g_strdup (spec->working_directory);
  c->stop_policy = spec->stop_policy;
  c->has_stall_timeout = spec->has_stall_timeout;
  c->stall_timeout = spec->stall_timeout;
  c->transcript = g_strdup (spec->transcript);
  return c;
}

static BroRunningProcess *
launcher_start (BroProcessLauncher *l, const BroProcessSpec *spec, BroBroError **error)
{
  TestScriptedLauncher *self = TEST_SCRIPTED_LAUNCHER (l);
  TestScript *script = g_object_new (TEST_TYPE_SCRIPT, NULL);
  g_ptr_array_add (self->started, spec_copy (spec));
  script->lines = g_strdupv (self->lines);
  script->exit_code = self->exit_code;
  script->write_file_in = g_strdup (self->write_file_in);
  script->cancel_after = self->cancel_after;
  script->cancel = self->cancel;
  g_set_object (&self->last, script);
  return BRO_RUNNING_PROCESS (script);
}

static void
test_scripted_launcher_finalize (GObject *o)
{
  TestScriptedLauncher *self = TEST_SCRIPTED_LAUNCHER (o);
  g_strfreev (self->lines);
  g_free (self->write_file_in);
  g_ptr_array_unref (self->started);
  g_clear_object (&self->last);
  G_OBJECT_CLASS (test_scripted_launcher_parent_class)->finalize (o);
}

static void test_scripted_launcher_class_init (TestScriptedLauncherClass *k) { G_OBJECT_CLASS (k)->finalize = test_scripted_launcher_finalize; }

static void
test_scripted_launcher_init (TestScriptedLauncher *self)
{
  self->started = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_process_spec_free);
  self->cancel_after = -1;
}

static void test_scripted_launcher_iface_init (BroProcessLauncherInterface *iface) { iface->start = launcher_start; }

/* An isolation that records what happens to its leases. */
#define TEST_TYPE_RECORDING_ISOLATION (test_recording_isolation_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingIsolation, test_recording_isolation, TEST, RECORDING_ISOLATION, GObject)

struct _TestRecordingIsolation {
  GObject parent_instance;
  GPtrArray *log;
};

#define TEST_TYPE_RECORDING_LEASE (test_recording_lease_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingLease, test_recording_lease, TEST, RECORDING_LEASE, GObject)

struct _TestRecordingLease {
  GObject parent_instance;
  TestRecordingIsolation *owner;
  char *home;
  char *profile;
};

static void test_recording_lease_iface_init (BroIsolationLeaseInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestRecordingLease, test_recording_lease, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_ISOLATION_LEASE, test_recording_lease_iface_init))

static GHashTable *
rl_environment (BroIsolationLease *l)
{
  GHashTable *env = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (env, g_strdup ("HOME"), g_strdup (TEST_RECORDING_LEASE (l)->home));
  return env;
}

static char *rl_profile (BroIsolationLease *l) { return g_strdup (TEST_RECORDING_LEASE (l)->profile); }
static void rl_first (BroIsolationLease *l) { g_ptr_array_add (TEST_RECORDING_LEASE (l)->owner->log, g_strdup ("firstOutput")); }
static void rl_release (BroIsolationLease *l) { g_ptr_array_add (TEST_RECORDING_LEASE (l)->owner->log, g_strdup ("release")); }

static void
test_recording_lease_finalize (GObject *o)
{
  TestRecordingLease *self = TEST_RECORDING_LEASE (o);
  g_object_unref (self->owner);
  g_free (self->home);
  g_free (self->profile);
  G_OBJECT_CLASS (test_recording_lease_parent_class)->finalize (o);
}

static void test_recording_lease_class_init (TestRecordingLeaseClass *k) { G_OBJECT_CLASS (k)->finalize = test_recording_lease_finalize; }
static void test_recording_lease_init (TestRecordingLease *self) {}

static void
test_recording_lease_iface_init (BroIsolationLeaseInterface *iface)
{
  iface->environment = rl_environment;
  iface->profile_path = rl_profile;
  iface->first_output = rl_first;
  iface->release = rl_release;
}

static void test_recording_isolation_iface_init (BroSettingsIsolationInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestRecordingIsolation, test_recording_isolation, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_SETTINGS_ISOLATION, test_recording_isolation_iface_init))

static BroIsolationLease *
ri_prepare (BroSettingsIsolation *i, const BroMakemkvRunSettings *settings, BroBroError **error)
{
  TestRecordingIsolation *self = TEST_RECORDING_ISOLATION (i);
  TestRecordingLease *lease = g_object_new (TEST_TYPE_RECORDING_LEASE, NULL);
  g_ptr_array_add (self->log, g_strdup ("prepare"));
  lease->owner = g_object_ref (self);
  lease->home = g_strdup (settings->work_directory);
  lease->profile = settings->profile_xml ? g_strconcat (settings->work_directory, "/profile.mmcp.xml", NULL) : NULL;
  return BRO_ISOLATION_LEASE (lease);
}

static void
test_recording_isolation_finalize (GObject *o)
{
  g_ptr_array_unref (TEST_RECORDING_ISOLATION (o)->log);
  G_OBJECT_CLASS (test_recording_isolation_parent_class)->finalize (o);
}

static void test_recording_isolation_class_init (TestRecordingIsolationClass *k) { G_OBJECT_CLASS (k)->finalize = test_recording_isolation_finalize; }
static void test_recording_isolation_init (TestRecordingIsolation *self) { self->log = g_ptr_array_new_with_free_func (g_free); }
static void test_recording_isolation_iface_init (BroSettingsIsolationInterface *iface) { iface->prepare = ri_prepare; }

/* Finds makemkvcon at path (NULL: nowhere). */
#define TEST_TYPE_FIXED_LOCATOR (test_fixed_locator_get_type ())
G_DECLARE_FINAL_TYPE (TestFixedLocator, test_fixed_locator, TEST, FIXED_LOCATOR, GObject)

struct _TestFixedLocator {
  GObject parent_instance;
  char *path;    /* makemkvcon */
  char *apprise;
};

static void test_fixed_locator_iface_init (BroToolLocatorInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFixedLocator, test_fixed_locator, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_TOOL_LOCATOR, test_fixed_locator_iface_init))

static BroToolInfo *
fl_locate (BroToolLocator *l, BroToolKind tool)
{
  TestFixedLocator *self = TEST_FIXED_LOCATOR (l);
  BroToolInfo *info = bro_tool_info_new ();
  info->tool = tool;
  if (tool == BRO_TOOL_KIND_MAKEMKVCON && self->path)
    info->path = g_strdup (self->path);
  else if (tool == BRO_TOOL_KIND_APPRISE && self->apprise)
    info->path = g_strdup (self->apprise);
  else
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (tool)));
      info->why = bro_bro_message_new (BRO_MSG_TOOL_MISSING, params, BRO_SEVERITY_WARNING);
    }
  return info;
}

static void
test_fixed_locator_finalize (GObject *o)
{
  g_free (TEST_FIXED_LOCATOR (o)->path);
  g_free (TEST_FIXED_LOCATOR (o)->apprise);
  G_OBJECT_CLASS (test_fixed_locator_parent_class)->finalize (o);
}
static void test_fixed_locator_class_init (TestFixedLocatorClass *k) { G_OBJECT_CLASS (k)->finalize = test_fixed_locator_finalize; }
static void test_fixed_locator_init (TestFixedLocator *self) {}
static void test_fixed_locator_iface_init (BroToolLocatorInterface *iface) { iface->locate = fl_locate; }

#define TEST_TYPE_COUNTING_SINK (test_counting_sink_get_type ())
G_DECLARE_FINAL_TYPE (TestCountingSink, test_counting_sink, TEST, COUNTING_SINK, GObject)

struct _TestCountingSink {
  GObject parent_instance;
  int events;
};

static void test_counting_sink_iface_init (BroRunSinkInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestCountingSink, test_counting_sink, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_RUN_SINK, test_counting_sink_iface_init))
static void cs_event (BroRunSink *s, const BroRobotEvent *e) { TEST_COUNTING_SINK (s)->events++; }
static void test_counting_sink_class_init (TestCountingSinkClass *k) {}
static void test_counting_sink_init (TestCountingSink *self) {}
static void test_counting_sink_iface_init (BroRunSinkInterface *iface) { iface->event = cs_event; }

static char *
shown_text (const char *text)
{
  g_autofree char *home = g_strconcat (fs_root, "/home", NULL);
  g_autoptr (GString) s = g_string_new (text ? text : "");
  g_string_replace (s, home, "<work>", 0);
  g_string_replace (s, fs_root, "<root>", 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static BroMakemkvSource *
case_source (BroJsonValue *v)
{
  BroJsonValue *d = bro_json_value_member (v, "drive");
  if (d)
    return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_DRIVE, (int) bro_json_value_get_integer (bro_json_value_member (d, "index"), 0),
                                   bro_json_value_get_string (bro_json_value_member (d, "device"), ""), NULL);
  if (bro_json_value_member (v, "iso"))
    return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_ISO, 0, NULL, bro_json_value_get_string (bro_json_value_member (v, "iso"), ""));
  return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_FILE, 0, NULL, bro_json_value_get_string (bro_json_value_member (v, "file"), ""));
}

static GStrv
case_lines (BroJsonValue *given)
{
  const char *transcript = bro_json_value_get_string (bro_json_value_member (given, "transcript"), NULL);
  BroJsonValue *lines = bro_json_value_member (given, "lines");
  GPtrArray *out = g_ptr_array_new ();
  if (transcript)
    {
      g_autofree char *text = bro_test_fixture_text (transcript, NULL);
      g_auto (GStrv) split = g_strsplit (text, "\n", -1);
      for (guint i = 0; split[i]; i++)
        if (*split[i])
          g_ptr_array_add (out, g_strdup (split[i]));
    }
  for (guint i = 0; lines && i < bro_json_value_length (lines); i++)
    g_ptr_array_add (out, g_strdup (bro_json_value_get_string (bro_json_value_at (lines, i), "")));
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

static char *
joined_strings (BroJsonValue *array)
{
  g_autoptr (GPtrArray) parts = g_ptr_array_new ();
  for (guint i = 0; i < bro_json_value_length (array); i++)
    g_ptr_array_add (parts, (gpointer) bro_json_value_get_string (bro_json_value_at (array, i), ""));
  g_ptr_array_add (parts, NULL);
  return g_strjoinv ("|", (char **) parts->pdata);
}

static gboolean
makemkv_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestScriptedLauncher) launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  g_autoptr (TestRecordingIsolation) isolation = g_object_new (TEST_TYPE_RECORDING_ISOLATION, NULL);
  g_autoptr (TestFixedLocator) locator = g_object_new (TEST_TYPE_FIXED_LOCATOR, NULL);
  g_autoptr (TestCountingSink) sink = g_object_new (TEST_TYPE_COUNTING_SINK, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroMakemkvTool) tool = NULL;
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  g_autoptr (BroMakemkvInvocation) invocation = bro_makemkv_invocation_new ();
  g_autoptr (BroMakemkvSource) source = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroMakemkvRun) run_owned = NULL;
  g_autoptr (BroListingRun) listing = NULL;
  g_autoptr (GPtrArray) drives = NULL;
  g_autofree char *destination = NULL;
  BroMakemkvRun *run = NULL;
  BroProcessSpec *spec;
  const char *call = bro_json_value_get_string (bro_json_value_member (given, "call"), "");
  BroJsonValue *existing = bro_json_value_member (given, "existing");
  BroJsonValue *mk = bro_json_value_member (given, "makemkvcon");
  BroJsonValue *v;
  remove_tree (fs_root);
  {
    g_autofree char *home = g_build_filename (fs_root, "home", NULL);
    g_mkdir_with_parents (home, 0755);
  }
  for (guint i = 0; existing && i < bro_json_value_length (existing); i++)
    {
      g_autofree char *path = fs_path (bro_json_value_get_string (bro_json_value_at (existing, i), ""));
      g_autofree char *dir = g_path_get_dirname (path);
      g_mkdir_with_parents (dir, 0755);
      g_file_set_contents (path, "", 0, NULL);
    }
  if ((v = bro_json_value_member (given, "destination")))
    {
      destination = fs_path (bro_json_value_get_string (v, ""));
      g_mkdir_with_parents (destination, 0755);
    }
  launcher->lines = case_lines (given);
  launcher->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (given, "exitCode"), 0);
  if (bro_json_value_get_bool (bro_json_value_member (given, "writesFile"), FALSE))
    launcher->write_file_in = g_strdup (destination);
  launcher->cancel_after = (int) bro_json_value_get_integer (bro_json_value_member (given, "cancelAfterLines"), -1);
  launcher->cancel = cancel;
  locator->path = mk && mk->kind == BRO_JSON_VALUE_NULL ? NULL : g_strdup ("/opt/makemkvcon");
  tool = bro_makemkv_tool_new (BRO_PROCESS_LAUNCHER (launcher), BRO_FILE_SYSTEM (fs), BRO_SETTINGS_ISOLATION (isolation), BRO_TOOL_LOCATOR (locator));
  invocation->options.min_length_seconds = (int) bro_json_value_get_integer (bro_json_value_member (bro_json_value_member (given, "options"), "minLengthSeconds"), -1);
  invocation->settings->profile_xml = g_strdup (bro_json_value_get_string (bro_json_value_member (given, "profileXml"), NULL));
  invocation->settings->data_dir = g_strdup ("/data");
  invocation->settings->work_directory = g_build_filename (fs_root, "home", NULL);
  if ((v = bro_json_value_member (given, "stallTimeout")))
    {
      invocation->has_stall_timeout = TRUE;
      invocation->stall_timeout.seconds = (double) bro_json_value_get_integer (v, 0);
    }
  if ((v = bro_json_value_member (given, "transcriptFile")))
    invocation->transcript = fs_path (bro_json_value_get_string (v, ""));
  if ((v = bro_json_value_member (given, "source")))
    source = case_source (v);

  if (g_str_equal (call, "scanDrives"))
    drives = bro_makemkv_tool_scan_drives (tool, cancel, &error);
  else if (g_str_equal (call, "listing"))
    {
      listing = bro_makemkv_tool_listing (tool, source, invocation, BRO_RUN_SINK (sink), cancel, &error);
      run = listing ? listing->run : NULL;
    }
  else if (g_str_equal (call, "rip"))
    run = run_owned = bro_makemkv_tool_rip (tool, source, bro_json_value_get_string (bro_json_value_member (given, "title"), ""), destination,
                                            invocation, BRO_RUN_SINK (sink), cancel, &error);
  else if (g_str_equal (call, "backup"))
    run = run_owned = bro_makemkv_tool_backup (tool, source, bro_json_value_get_bool (bro_json_value_member (given, "decrypt"), FALSE), destination,
                                               invocation, BRO_RUN_SINK (sink), cancel, &error);
  else
    return FALSE;
  bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (expect, "error"), NULL), error ? error->code : NULL);

  spec = launcher->started->len ? launcher->started->pdata[0] : NULL;
  if ((v = bro_json_value_member (expect, "launched")) && !bro_json_value_get_bool (v, TRUE) && spec)
    bro_test_fail (failures, id, "launched");
  if ((v = bro_json_value_member (expect, "arguments")))
    {
      g_autofree char *want = joined_strings (v);
      g_autofree char *got_raw = spec ? g_strjoinv ("|", spec->arguments) : NULL;
      g_autofree char *got = shown_text (got_raw);
      bro_test_same_string (failures, id, "arguments", want, got);
    }
  if ((v = bro_json_value_member (expect, "environment")))
    {
      g_autoptr (GString) want = g_string_new (NULL);
      g_autoptr (GString) got = g_string_new (NULL);
      g_autoptr (GList) keys = spec ? g_list_sort (g_hash_table_get_keys (spec->environment), (GCompareFunc) strcmp) : NULL;
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_string_append_printf (want, "%s=%s;", (char *) v->keys->pdata[i], bro_json_value_get_string (bro_json_value_at (v, i), ""));
      for (GList *k = keys; k; k = k->next)
        {
          g_autofree char *value = shown_text (g_hash_table_lookup (spec->environment, k->data));
          g_string_append_printf (got, "%s=%s;", (char *) k->data, value);
        }
      bro_test_same_string (failures, id, "environment", want->str, got->str);
    }
  if ((v = bro_json_value_member (expect, "workingDirectory")))
    {
      g_autofree char *got = spec && spec->working_directory ? shown_text (spec->working_directory) : NULL;
      bro_test_same_string (failures, id, "workingDirectory", bro_json_value_get_string (v, NULL), got);
    }
  if ((v = bro_json_value_member (expect, "stopPolicy")))
    bro_test_same_string (failures, id, "stopPolicy", bro_json_value_get_string (v, ""), spec ? bro_stop_policy_to_wire (spec->stop_policy) : NULL);
  if ((v = bro_json_value_member (expect, "stallTimeout")) && (!spec || !spec->has_stall_timeout || spec->stall_timeout.seconds != bro_json_value_get_integer (v, 0)))
    bro_test_fail (failures, id, "stallTimeout");
  if ((v = bro_json_value_member (expect, "transcriptFile")))
    {
      g_autofree char *got = spec ? shown_text (spec->transcript) : NULL;
      bro_test_same_string (failures, id, "transcriptFile", bro_json_value_get_string (v, ""), got);
    }
  if ((v = bro_json_value_member (expect, "lease")))
    {
      g_autofree char *want = joined_strings (v);
      g_autofree char *got = NULL;
      g_ptr_array_add (isolation->log, NULL);
      got = g_strjoinv ("|", (char **) isolation->log->pdata);
      g_ptr_array_remove_index (isolation->log, isolation->log->len - 1);
      bro_test_same_string (failures, id, "lease", want, got);
    }
  if ((v = bro_json_value_member (expect, "stopped")) && bro_json_value_get_bool (v, FALSE) != (launcher->last && launcher->last->stopped))
    bro_test_fail (failures, id, "stopped");
  if ((v = bro_json_value_member (expect, "linesRead")) && (!launcher->last || launcher->last->handed != bro_json_value_get_integer (v, -1)))
    bro_test_fail (failures, id, "linesRead: %d", launcher->last ? launcher->last->handed : -1);
  if ((v = bro_json_value_member (expect, "drives")))
    {
      g_autoptr (GString) want = g_string_new (NULL);
      g_autoptr (GString) got = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        {
          BroJsonValue *d = bro_json_value_at (v, i);
          g_string_append_printf (want, "%" G_GINT64_FORMAT " %s %s;", bro_json_value_get_integer (bro_json_value_at (d, 0), -1),
                                  bro_json_value_get_string (bro_json_value_at (d, 1), ""), bro_json_value_get_string (bro_json_value_at (d, 2), ""));
        }
      for (guint i = 0; drives && i < drives->len; i++)
        {
          BroMakemkvDrive *d = drives->pdata[i];
          g_string_append_printf (got, "%d %s %s;", d->index, bro_drive_state_to_wire (d->state), d->device);
        }
      bro_test_same_string (failures, id, "drives", want->str, got->str);
    }
  if ((v = bro_json_value_member (expect, "listing")))
    {
      bro_test_same_string (failures, id, "volumeName", bro_json_value_get_string (bro_json_value_member (v, "volumeName"), ""),
                            listing ? listing->listing->volume_name : NULL);
      if (!listing || (gint64) listing->listing->titles->len != bro_json_value_get_integer (bro_json_value_member (v, "titles"), -1))
        bro_test_fail (failures, id, "titles");
    }
  if ((v = bro_json_value_member (expect, "status")))
    bro_test_same_string (failures, id, "status", bro_json_value_get_string (v, ""), run ? bro_status_word_to_wire (run->outcome->status) : NULL);
  if ((v = bro_json_value_member (expect, "errorContains")) && v->kind == BRO_JSON_VALUE_STRING)
    {
      g_autoptr (BroJsonValue) json = run && run->outcome->error ? bro_bro_message_to_json (run->outcome->error) : NULL;
      g_autofree char *text = json ? bro_test_english (json) : NULL;
      if (!text || !strstr (text, bro_json_value_get_string (v, "")))
        bro_test_fail (failures, id, "error %s doesn't contain %s", text ? text : "(none)", bro_json_value_get_string (v, ""));
    }
  if ((v = bro_json_value_member (expect, "errorCode")))
    bro_test_same_string (failures, id, "errorCode", bro_json_value_get_string (v, ""),
                          run && run->outcome->error ? bro_message_code_wire (run->outcome->error->code) : NULL);
  if ((v = bro_json_value_member (expect, "version")))
    bro_test_same_string (failures, id, "version", bro_json_value_get_string (v, ""), run ? run->version : NULL);
  if ((v = bro_json_value_member (expect, "debugLog")))
    bro_test_same_string (failures, id, "debugLog", bro_json_value_get_string (v, ""), run ? run->outcome->debug_log : NULL);
  if (run && sink->events == 0)
    bro_test_fail (failures, id, "the sink heard nothing");
  return TRUE;
}

static void
test_makemkv_tool (void)
{
  bro_test_run_cases ("adapters/makemkv-tool.cases.json", makemkv_case);
}

/* The adapters together with the real makemkvcon on a disc image: only when BROMELIA_TEST_ISO names one (CLAUDE.md,
 * "Real-disc tests"; makemkvcon must be on PATH). No drive is touched, and the settings are empty, so the owner's
 * MakeMKV settings and key are never read. */
static void
test_real_iso (void)
{
  const char *iso = g_getenv ("BROMELIA_TEST_ISO");
  g_autoptr (BroPlatformFileSystem) fs = NULL;
  g_autoptr (BroPlatformProcessLauncher) launcher = NULL;
  g_autoptr (BroHomeDirIsolation) isolation = NULL;
  g_autoptr (BroSystemToolLocator) locator = NULL;
  g_autoptr (BroMakemkvTool) tool = NULL;
  g_autoptr (GHashTable) configured = NULL;
  g_autoptr (GHashTable) candidates = NULL;
  g_autoptr (GHashTable) names = NULL;
  g_autoptr (BroMakemkvInvocation) invocation = NULL;
  g_autoptr (BroMakemkvSource) source = NULL;
  g_autoptr (BroListingRun) result = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (TestCountingSink) sink = NULL;
  g_auto (GStrv) path = NULL;
  g_autofree char *work = NULL;
  g_autofree char *fingerprint = NULL;
  g_autofree char *transcript = NULL;
  g_autofree char *transcript_path = NULL;
  if (!iso || !*iso)
    {
      g_test_skip ("BROMELIA_TEST_ISO isn't set");
      return;
    }
  fs = bro_platform_file_system_new ();
  launcher = bro_platform_process_launcher_new ();
  work = g_build_filename (fs_root, "iso", NULL);
  remove_tree (fs_root);
#ifdef __APPLE__
  isolation = bro_home_dir_isolation_new (BRO_FILE_SYSTEM (fs), BRO_HOME_LAYOUT_MACOS);
#else
  isolation = bro_home_dir_isolation_new (BRO_FILE_SYSTEM (fs), BRO_HOME_LAYOUT_LINUX);
#endif
  configured = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  candidates = bro_platform_tool_paths_candidates (g_get_home_dir ());
  names = bro_platform_tool_paths_names ();
  path = g_strsplit (g_getenv ("PATH") ? g_getenv ("PATH") : "", ":", -1);
  locator = bro_system_tool_locator_new (BRO_FILE_SYSTEM (fs), configured, candidates, names, (const char *const *) path, g_get_home_dir ());
  tool = bro_makemkv_tool_new (BRO_PROCESS_LAUNCHER (launcher), BRO_FILE_SYSTEM (fs), BRO_SETTINGS_ISOLATION (isolation), BRO_TOOL_LOCATOR (locator));
  invocation = bro_makemkv_invocation_new ();
  invocation->settings->data_dir = g_strdup ("");
  invocation->settings->work_directory = g_build_filename (work, "home", NULL);
  invocation->has_stall_timeout = TRUE;
  invocation->stall_timeout.seconds = 300;
  transcript_path = g_build_filename (work, "makemkv.txt", NULL);
  invocation->transcript = g_strdup (transcript_path);
  source = bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_ISO, 0, NULL, iso);
  sink = g_object_new (TEST_TYPE_COUNTING_SINK, NULL);
  result = bro_makemkv_tool_listing (tool, source, invocation, BRO_RUN_SINK (sink), NULL, &error);
  g_assert_null (error);
  g_assert_nonnull (result);
  g_assert_cmpstr (bro_status_word_to_wire (result->run->outcome->status), ==, "success");
  g_assert_cmpuint (result->listing->titles->len, >, 0);
  g_assert_cmpint (sink->events, >, 0);
  fingerprint = bro_fingerprint_of (result->listing);
  g_test_message ("%u titles, %s, fingerprint %s", result->listing->titles->len, result->run->version, fingerprint);
  g_assert_true (fingerprint && (g_str_has_prefix (fingerprint, "v1:c13d733d") || g_str_has_prefix (fingerprint, "v1:742cbae9")));
  g_assert_true (g_file_get_contents (transcript_path, &transcript, NULL, NULL));
  g_assert_true (g_str_has_prefix (transcript, "==== "));
  g_assert_nonnull (strstr (transcript, " exit status 0\n"));
}

#include "test-store.inc"
#include "test-http.inc"
#include "test-metadata.inc"

/* ---- the clock ------------------------------------------------------------------------------------- */

static void
test_clock_now_and_sleep (void)
{
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  g_autoptr (BroBroError) error = NULL;
  BroDuration before;
  g_assert_cmpint (ABS (bro_clock_now (BRO_CLOCK (clock)).unix_milliseconds - g_get_real_time () / 1000), <, 1000);
  before = bro_clock_monotonic (BRO_CLOCK (clock));
  g_assert_true (bro_clock_sleep (BRO_CLOCK (clock), (BroDuration) { 0.2 }, cancel, &error));
  g_assert_cmpfloat (bro_clock_monotonic (BRO_CLOCK (clock)).seconds - before.seconds, >=, 0.15);
}

static gpointer
cancel_soon (gpointer data)
{
  g_usleep (G_USEC_PER_SEC / 20);
  bro_cancellation_token_cancel (data);
  return NULL;
}

static void
test_clock_cancelled_sleep (void)
{
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  g_autoptr (BroBroError) error = NULL;
  GThread *t = g_thread_new ("cancel", cancel_soon, cancel);
  gint64 t0 = g_get_monotonic_time ();
  g_assert_false (bro_clock_sleep (BRO_CLOCK (clock), (BroDuration) { 30 }, cancel, &error));
  g_thread_join (t);
  g_assert_cmpstr (error->code, ==, "job.cancelled");
  g_assert_cmpfloat (since (t0), <, 5);
}

static void
count_up (gpointer data)
{
  g_atomic_int_inc ((int *) data);
}

static void
test_clock_timers (void)
{
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  BroTimerSchedule at = { BRO_TIMER_SCHEDULE_AT, { bro_clock_now (BRO_CLOCK (clock)).unix_milliseconds + 100 }, { 0 } };
  BroTimerSchedule every = { BRO_TIMER_SCHEDULE_EVERY, { 0 }, { 0.05 } };
  static int fired, ticks;
  BroTimerHandle *handle;
  int after;
  /* The handle is dropped straight away: the timer still fires. */
  g_object_unref (bro_clock_timer (BRO_CLOCK (clock), &at, count_up, &fired, NULL));
  for (int i = 0; i < 100 && !g_atomic_int_get (&fired); i++)
    g_usleep (G_USEC_PER_SEC / 20);
  g_assert_cmpint (g_atomic_int_get (&fired), ==, 1);

  handle = bro_clock_timer (BRO_CLOCK (clock), &every, count_up, &ticks, NULL);
  for (int i = 0; i < 100 && g_atomic_int_get (&ticks) < 3; i++)
    g_usleep (G_USEC_PER_SEC / 20);
  bro_timer_handle_cancel (handle);
  g_object_unref (handle);
  g_assert_cmpint (g_atomic_int_get (&ticks), >=, 3);
  g_usleep (G_USEC_PER_SEC / 10);
  after = g_atomic_int_get (&ticks);
  g_usleep (G_USEC_PER_SEC / 5);
  g_assert_cmpint (g_atomic_int_get (&ticks), ==, after);
}

int
main (int argc, char **argv)
{
  int status;
  g_autofree char *cleanup = NULL;
  g_test_init (&argc, &argv, NULL);
  scratch = g_dir_make_tmp ("bromelia-adapters-XXXXXX", NULL);
  {
    char *real = realpath (scratch, NULL); /* macOS: /var → /private/var, so error paths match */
    fs_root = g_build_filename (real, "fs", NULL);
    store_dir = g_build_filename (real, "store", NULL);
    g_mkdir_with_parents (store_dir, 0755);
    free (real);
  }
  g_test_add_func ("/process/stall-stops", test_stall_stops);
  g_test_add_func ("/process/busy-not-stopped", test_busy_not_stopped);
  g_test_add_func ("/process/cancel-escalates", test_cancel_escalates);
  g_test_add_func ("/process/terminate-first", test_terminate_first);
  g_test_add_func ("/process/output-drained", test_output_drained);
  g_test_add_func ("/process/process-group", test_process_group);
  g_test_add_func ("/process/transcript", test_transcript);
  g_test_add_func ("/process/streams", test_streams);
  g_test_add_func ("/process/could-not-start", test_could_not_start);
  g_test_add_func ("/process/interpreter", test_interpreter);
  g_test_add_func ("/file-system/cases", test_fs_cases);
  g_test_add_func ("/file-system/durable-and-uncached", test_fs_durable_and_uncached);
  g_test_add_func ("/file-system/volume", test_fs_volume);
  g_test_add_func ("/settings-isolation/home", test_home_isolation);
  g_test_add_func ("/tool-locator/cases", test_tool_locator);
  g_test_add_func ("/tool-locator/platform-paths", test_tool_paths);
  g_test_add_func ("/makemkv-tool/cases", test_makemkv_tool);
  g_test_add_func ("/makemkv-tool/real-iso", test_real_iso);
  g_test_add_func ("/store/cases", test_store_cases);
  g_test_add_func ("/store/round-trips", test_store_round_trips);
  g_test_add_func ("/store/transaction", test_store_transaction);
  g_test_add_func ("/store/schema", test_store_schema);
  g_test_add_func ("/http/client", test_http_client);
  g_test_add_func ("/http/notification-sender", test_notification_sender);
  g_test_add_func ("/http/beta-key-source", test_beta_key_source);
  g_test_add_func ("/http/metadata-client", test_metadata_client);
  g_test_add_func ("/clock/now-and-sleep", test_clock_now_and_sleep);
  g_test_add_func ("/clock/cancelled-sleep", test_clock_cancelled_sleep);
  g_test_add_func ("/clock/timers", test_clock_timers);
  status = g_test_run ();
  cleanup = g_strdup_printf ("rm -rf '%s'", scratch);
  g_spawn_command_line_sync (cleanup, NULL, NULL, NULL, NULL);
  g_free (scratch);
  g_free (fs_root);
  g_free (store_dir);
  return status;
}
