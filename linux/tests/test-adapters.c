/* test-adapters.c: the Adapters layer on the real OS. The process cases are those of
 * shared/fixtures/adapters/platform-adapters.contract.json. */
#include "bro-test-fixtures.h"

#include "bro-platform-process-launcher.h"
#include "bro-system-clock.h"

#include <errno.h>
#include <glib/gstdio.h>
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
  g_test_add_func ("/clock/now-and-sleep", test_clock_now_and_sleep);
  g_test_add_func ("/clock/cancelled-sleep", test_clock_cancelled_sleep);
  g_test_add_func ("/clock/timers", test_clock_timers);
  status = g_test_run ();
  cleanup = g_strdup_printf ("rm -rf '%s'", scratch);
  g_spawn_command_line_sync (cleanup, NULL, NULL, NULL, NULL);
  g_free (scratch);
  return status;
}
