/* test-launcher-abuse.inc: launchers against misbehaving tools (review follow-ups, failure testing), included by
 * test-adapters.c. Opt-in (they take a minute and a lot of memory): BROMELIA_TEST_ABUSE=1. Each reports what it
 * measured with g_test_message. */
#include <sys/resource.h>

/* ASan keeps freed memory in its quarantine, so the memory checks below only hold without it. */
#if defined(__SANITIZE_ADDRESS__)
#define ABUSE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ABUSE_ASAN 1
#endif
#endif
#ifndef ABUSE_ASAN
#define ABUSE_ASAN 0
#endif

static gboolean
abuse_enabled (void)
{
  if (g_strcmp0 (g_getenv ("BROMELIA_TEST_ABUSE"), "1") == 0)
    return TRUE;
  g_test_skip ("set BROMELIA_TEST_ABUSE=1 to run");
  return FALSE;
}

/* This process's peak resident memory, in MB. */
static double
peak_mb (void)
{
  struct rusage u;
  getrusage (RUSAGE_SELF, &u);
#ifdef __APPLE__
  return u.ru_maxrss / 1048576.0;
#else
  return u.ru_maxrss / 1024.0;
#endif
}

/* 300 MB of random bytes, read as they come: every line arrives cut to its limit, memory stays small. */
static void
test_abuse_binary_flood (void)
{
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (BroRunningProcess) p = NULL;
  BroOutputLine *line;
  guint64 lines = 0;
  gsize longest = 0;
  double before;
  BroProcessExit exit;
  if (!abuse_enabled ())
    return;
  spec = spec_new ("/bin/sh", "-c", "head -c 300000000 /dev/urandom", NULL);
  before = peak_mb ();
  p = start (spec);
  while ((line = bro_running_process_lines (p)) != NULL)
    {
      lines++;
      longest = MAX (longest, strlen (line->text));
      bro_output_line_free (line);
    }
  exit = bro_running_process_wait (p);
  g_test_message ("binary flood: %" G_GUINT64_FORMAT " lines, longest %" G_GSIZE_FORMAT " bytes, peak memory %.0f MB -> %.0f MB",
                  lines, longest, before, peak_mb ());
  g_assert_cmpint (exit.status, ==, 0);
  g_assert_cmpuint (longest, <=, 65536 * 3 + 16); /* bad bytes become U+FFFD (3 bytes) */
  if (!ABUSE_ASAN)
    g_assert_cmpfloat (peak_mb () - before, <, 200);
}

/* A tool printing 65000-byte lines while the reader has stopped: the queue holds no more than its byte limit, the tool
 * waits (backpressure), and a stop still ends it. */
static void
test_abuse_long_lines_reader_stops (void)
{
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (BroRunningProcess) p = NULL;
  BroOutputLine *line;
  double before, held;
  gint64 t0;
  guint64 drained = 0;
  BroProcessExit exit;
  if (!abuse_enabled ())
    return;
  spec = spec_new ("/bin/sh", "-c", "exec yes \"$(head -c 65000 /dev/zero | tr '\\0' x)\"", NULL);
  spec->stop_policy = BRO_STOP_POLICY_TERMINATE_FIRST;
  before = peak_mb ();
  p = start (spec);
  line = bro_running_process_lines (p);
  bro_output_line_free (line);
  g_usleep (8 * G_USEC_PER_SEC); /* not reading */
  held = peak_mb ();
  t0 = g_get_monotonic_time ();
  bro_running_process_stop (p, BRO_STOP_REASON_CANCELLED);
  while ((line = bro_running_process_lines (p)) != NULL)
    {
      drained++;
      bro_output_line_free (line);
    }
  exit = bro_running_process_wait (p);
  g_test_message ("long lines, reader stopped: peak memory %.0f MB -> %.0f MB while not reading; stop took %.1f s; %" G_GUINT64_FORMAT
                  " lines drained", before, held, since (t0), drained);
  g_assert_true (exit.cancelled);
  g_assert_cmpfloat (since (t0), <, 10);
  if (!ABUSE_ASAN)
    g_assert_cmpfloat (held - before, <, 100); /* BRO_QUEUED_BYTES, not 10000 lines of 64 KiB */
}

/* Five generations of shells that all ignore INT and TERM: a stop ends every one of them (KILL after the grace period). */
static void
test_abuse_deep_tree_ignores_term (void)
{
  g_autofree char *script = g_build_filename (scratch, "deep.sh", NULL);
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (BroRunningProcess) p = NULL;
  g_autoptr (GArray) pids = g_array_new (FALSE, FALSE, sizeof (int));
  BroOutputLine *line;
  gint64 t0;
  BroProcessExit exit;
  if (!abuse_enabled ())
    return;
  g_assert_true (g_file_set_contents (script,
                                      "trap '' INT TERM\n"
                                      "if [ \"$1\" -gt 0 ]; then /bin/sh \"$0\" $(($1 - 1)) & fi\n"
                                      "echo \"$$\"\n"
                                      "sleep 120\n",
                                      -1, NULL));
  spec = spec_new ("/bin/sh", script, "4", NULL);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  p = start (spec);
  while (pids->len < 5 && (line = bro_running_process_lines (p)) != NULL)
    {
      int pid = atoi (line->text);
      g_array_append_val (pids, pid);
      bro_output_line_free (line);
    }
  g_assert_cmpuint (pids->len, ==, 5);
  t0 = g_get_monotonic_time ();
  bro_running_process_stop (p, BRO_STOP_REASON_CANCELLED);
  while ((line = bro_running_process_lines (p)) != NULL)
    bro_output_line_free (line);
  exit = bro_running_process_wait (p);
  g_test_message ("deep tree ignoring INT and TERM: ended after %.1f s", since (t0));
  g_assert_true (exit.cancelled);
  g_assert_cmpfloat (since (t0), <, 12); /* interrupt first: INT, 5 s, TERM, 5 s, KILL */
  g_usleep (G_USEC_PER_SEC / 2);
  for (guint i = 0; i < pids->len; i++)
    if (kill (g_array_index (pids, int, i), 0) == 0)
      g_error ("pid %d (generation %u) is still running", g_array_index (pids, int, i), i);
}
