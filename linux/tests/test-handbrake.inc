/* test-handbrake.inc: BroHandBrake, included by test-adapters.c: shared/fixtures/adapters/handbrake.cases.json against a
 * scripted launcher, and the real HandBrakeCLI where it is installed (the Ubuntu VM). */

/* A clock whose timers fire as soon as they are set (when asked to), recording when they were for. */
#define TEST_TYPE_NULL_TIMER (test_null_timer_get_type ())
G_DECLARE_FINAL_TYPE (TestNullTimer, test_null_timer, TEST, NULL_TIMER, GObject)
struct _TestNullTimer { GObject parent_instance; };
static void test_null_timer_iface_init (BroTimerHandleInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestNullTimer, test_null_timer, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_TIMER_HANDLE, test_null_timer_iface_init))
static void nt_cancel (BroTimerHandle *t) {}
static void test_null_timer_class_init (TestNullTimerClass *k) {}
static void test_null_timer_init (TestNullTimer *self) {}
static void test_null_timer_iface_init (BroTimerHandleInterface *iface) { iface->cancel = nt_cancel; }

#define TEST_TYPE_FIRING_CLOCK (test_firing_clock_get_type ())
G_DECLARE_FINAL_TYPE (TestFiringClock, test_firing_clock, TEST, FIRING_CLOCK, GObject)
struct _TestFiringClock {
  GObject parent_instance;
  gboolean fire;
  GArray *timers; /* BroTimerSchedule */
};
static void test_firing_clock_iface_init (BroClockInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFiringClock, test_firing_clock, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, test_firing_clock_iface_init))
#define FIRING_CLOCK_AT G_GINT64_CONSTANT (1790000000000)
static BroInstant firing_now (BroClock *c) { return (BroInstant) { FIRING_CLOCK_AT }; }
static BroDuration firing_monotonic (BroClock *c) { return (BroDuration) { 0 }; }
static gboolean firing_sleep (BroClock *c, BroDuration d, BroCancellationToken *cancel, BroBroError **error) { return TRUE; }

static BroTimerHandle *
firing_timer (BroClock *c, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy)
{
  TestFiringClock *self = TEST_FIRING_CLOCK (c);
  g_array_append_val (self->timers, *schedule);
  if (self->fire)
    handler (data);
  if (destroy)
    destroy (data);
  return g_object_new (TEST_TYPE_NULL_TIMER, NULL);
}

static void test_firing_clock_finalize (GObject *o) { g_array_unref (TEST_FIRING_CLOCK (o)->timers); G_OBJECT_CLASS (test_firing_clock_parent_class)->finalize (o); }
static void test_firing_clock_class_init (TestFiringClockClass *k) { G_OBJECT_CLASS (k)->finalize = test_firing_clock_finalize; }
static void test_firing_clock_init (TestFiringClock *self) { self->timers = g_array_new (FALSE, TRUE, sizeof (BroTimerSchedule)); }

static void
test_firing_clock_iface_init (BroClockInterface *iface)
{
  iface->now = firing_now;
  iface->monotonic = firing_monotonic;
  iface->sleep = firing_sleep;
  iface->timer = firing_timer;
}

/* Keeps the progress values ("current/total/max") and the raw lines. */
#define TEST_TYPE_RECORDING_SINK (test_recording_sink_get_type ())
G_DECLARE_FINAL_TYPE (TestRecordingSink, test_recording_sink, TEST, RECORDING_SINK, GObject)
struct _TestRecordingSink {
  GObject parent_instance;
  GMutex lock;
  GPtrArray *progress, *raw;
};
static void test_recording_sink_iface_init (BroRunSinkInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestRecordingSink, test_recording_sink, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_RUN_SINK, test_recording_sink_iface_init))

static void
rs_event (BroRunSink *s, const BroRobotEvent *e)
{
  TestRecordingSink *self = TEST_RECORDING_SINK (s);
  g_mutex_lock (&self->lock);
  if (e->kind == BRO_ROBOT_EVENT_PROGRESS_VALUE)
    g_ptr_array_add (self->progress, g_strdup_printf ("%d/%d/%d", e->current, e->total, e->max));
  else if (e->kind == BRO_ROBOT_EVENT_RAW)
    g_ptr_array_add (self->raw, g_strdup (e->text));
  g_mutex_unlock (&self->lock);
}

static void
test_recording_sink_finalize (GObject *o)
{
  TestRecordingSink *self = TEST_RECORDING_SINK (o);
  g_ptr_array_unref (self->progress);
  g_ptr_array_unref (self->raw);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_recording_sink_parent_class)->finalize (o);
}

static void test_recording_sink_class_init (TestRecordingSinkClass *k) { G_OBJECT_CLASS (k)->finalize = test_recording_sink_finalize; }

static void
test_recording_sink_init (TestRecordingSink *self)
{
  g_mutex_init (&self->lock);
  self->progress = g_ptr_array_new_with_free_func (g_free);
  self->raw = g_ptr_array_new_with_free_func (g_free);
}

static void test_recording_sink_iface_init (BroRunSinkInterface *iface) { iface->event = rs_event; }

static char *
hb_r (const char *text)
{
  g_autofree char *home = g_build_filename (scratch, "home", NULL);
  g_autoptr (GString) s = g_string_new (text ? text : "");
  g_string_replace (s, "<root>", fs_root, 0);
  g_string_replace (s, "<home>", home, 0);
  g_string_replace (s, "<handbrake>", "/opt/HandBrakeCLI", 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

/* Joins strings with '|' for comparing lists. */
static char *
hb_joined (GPtrArray *items)
{
  g_autoptr (GString) s = g_string_new (NULL);
  for (guint i = 0; items && i < items->len; i++)
    g_string_append_printf (s, "%s%s", i ? "|" : "", (const char *) items->pdata[i]);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static char *
hb_expected (BroJsonValue *list, gboolean substitute)
{
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < bro_json_value_length (list); i++)
    {
      const char *text = bro_json_value_get_string (bro_json_value_at (list, i), "");
      g_ptr_array_add (items, substitute ? hb_r (text) : g_strdup (text));
    }
  return hb_joined (items);
}

static gboolean
handbrake_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestScriptedLauncher) launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  g_autoptr (TestFixedLocator) locator = g_object_new (TEST_TYPE_FIXED_LOCATOR, NULL);
  g_autoptr (TestFiringClock) clock = g_object_new (TEST_TYPE_FIRING_CLOCK, NULL);
  g_autoptr (TestRecordingSink) sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  g_autoptr (BroCancellationSource) cancel_source = bro_cancellation_source_new ();
  BroCancellationToken *cancel = bro_cancellation_source_token (cancel_source);
  g_autoptr (BroHandBrake) handbrake = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GStrvBuilder) lines = g_strv_builder_new ();
  g_autofree char *home = g_build_filename (scratch, "home", NULL);
  BroJsonValue *want_error = bro_json_value_member (expect, "error"), *v;
  const char *file = bro_json_value_get_string (bro_json_value_member (given, "linesFile"), NULL);
  int count = 0;
  remove_tree (fs_root);
  g_mkdir_with_parents (fs_root, 0755);
  launcher->stderr_at = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < bro_json_value_length (bro_json_value_member (given, "files")); i++)
    {
      g_autofree char *path = hb_r (bro_json_value_get_string (bro_json_value_at (bro_json_value_member (given, "files"), i), ""));
      g_autofree char *dir = g_path_get_dirname (path);
      g_mkdir_with_parents (dir, 0755);
      g_file_set_contents (path, "", 0, NULL);
    }
  if (file)
    {
      g_autofree char *text = bro_test_fixture_text (file, NULL);
      g_auto (GStrv) split = g_strsplit (text, "\n", -1);
      for (guint i = 0; split[i]; i++)
        if (*split[i])
          {
            g_array_append_val (launcher->stderr_at, count);
            g_strv_builder_add (lines, split[i]);
            count++;
          }
    }
  for (guint i = 0; i < bro_json_value_length (bro_json_value_member (given, "lines")); i++)
    {
      BroJsonValue *line = bro_json_value_at (bro_json_value_member (given, "lines"), i);
      const char *err = bro_json_value_get_string (bro_json_value_member (line, "stderr"), NULL);
      if (err)
        g_array_append_val (launcher->stderr_at, count);
      g_strv_builder_add (lines, err ? err : bro_json_value_get_string (line, ""));
      count++;
    }
  launcher->lines = g_strv_builder_end (lines);
  launcher->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (given, "exitCode"), 0);
  if (bro_json_value_get_bool (bro_json_value_member (given, "located"), TRUE))
    locator->handbrake = g_strdup ("/opt/HandBrakeCLI");
  clock->fire = bro_json_value_get_bool (bro_json_value_member (given, "timerFires"), FALSE);
  if (bro_json_value_get_bool (bro_json_value_member (given, "cancelled"), FALSE))
    bro_cancellation_source_cancel (cancel_source);
  handbrake = bro_hand_brake_new (BRO_PROCESS_LAUNCHER (launcher), BRO_TOOL_LOCATOR (locator), BRO_CLOCK (clock), home);

  if (g_str_equal (bro_json_value_get_string (bro_json_value_member (given, "op"), ""), "presets"))
    {
      g_auto (GStrv) presets = bro_hand_brake_presets (handbrake, cancel, &error);
      guint n = presets ? g_strv_length (presets) : 0;
      g_autoptr (GPtrArray) got = g_ptr_array_new ();
      g_autofree char *got_text = NULL;
      for (guint i = 0; i < n; i++)
        g_ptr_array_add (got, presets[i]);
      got_text = hb_joined (got);
      if ((v = bro_json_value_member (expect, "presets")))
        {
          g_autofree char *want = hb_expected (v, FALSE);
          bro_test_same_string (failures, id, "presets", want, got_text);
        }
      if ((v = bro_json_value_member (expect, "count")) && (gint64) n != bro_json_value_get_integer (v, -1))
        bro_test_fail (failures, id, "count %u", n);
      if ((v = bro_json_value_member (expect, "first")))
        bro_test_same_string (failures, id, "first", bro_json_value_get_string (v, ""), n ? presets[0] : NULL);
      if ((v = bro_json_value_member (expect, "last")))
        bro_test_same_string (failures, id, "last", bro_json_value_get_string (v, ""), n ? presets[n - 1] : NULL);
      for (guint i = 0; i < bro_json_value_length (bro_json_value_member (expect, "includes")); i++)
        {
          const char *p = bro_json_value_get_string (bro_json_value_at (bro_json_value_member (expect, "includes"), i), "");
          if (!presets || !g_strv_contains ((const char *const *) presets, p))
            bro_test_fail (failures, id, "missing preset %s", p);
        }
    }
  else
    {
      g_autoptr (GBytes) step_bytes = bro_json_value_encode_canonical (bro_json_value_member (given, "step"));
      g_autofree char *step_text = g_strndup (g_bytes_get_data (step_bytes, NULL), g_bytes_get_size (step_bytes));
      g_autofree char *step_r = hb_r (step_text);
      g_autoptr (BroJsonValue) step_json = bro_json_value_parse (step_r, -1);
      g_autoptr (BroStepDefinition) step = bro_step_definition_decode (step_json);
      g_autofree char *input = hb_r (bro_json_value_get_string (bro_json_value_member (given, "input"), ""));
      g_autofree char *output = hb_r (bro_json_value_get_string (bro_json_value_member (given, "output"), ""));
      BroHandBrakeRun run = { 0 };
      if (bro_hand_brake_encode (handbrake, step, input, output, BRO_RUN_SINK (sink), cancel, &run, &error))
        {
          if (want_error)
            bro_test_fail (failures, id, "expected an error");
          if (run.exit.status != bro_json_value_get_integer (bro_json_value_member (expect, "status"), -99))
            bro_test_fail (failures, id, "status %d", run.exit.status);
          if (run.timed_out != bro_json_value_get_bool (bro_json_value_member (expect, "timedOut"), FALSE))
            bro_test_fail (failures, id, "timedOut %d", run.timed_out);
        }
    }
  if (error)
    {
      bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (want_error, "code"), NULL), error->code);
      for (guint i = 0; want_error && i < bro_json_value_length (bro_json_value_member (want_error, "params")); i++)
        {
          BroJsonValue *params = bro_json_value_member (want_error, "params");
          const char *key = params->keys->pdata[i];
          g_autofree char *want = hb_r (bro_json_value_get_string (params->items->pdata[i], ""));
          bro_test_same_string (failures, id, key, want, bro_json_value_get_string (bro_json_value_member (error->params, key), NULL));
        }
    }
  if (!bro_json_value_get_bool (bro_json_value_member (expect, "started"), TRUE) && launcher->started->len)
    bro_test_fail (failures, id, "started");
  if ((v = bro_json_value_member (expect, "argv")))
    {
      g_autofree char *want = hb_expected (v, TRUE);
      g_autoptr (GPtrArray) got = g_ptr_array_new ();
      g_autofree char *got_text = NULL;
      if (launcher->started->len)
        {
          BroProcessSpec *spec = launcher->started->pdata[0];
          g_ptr_array_add (got, spec->executable);
          for (guint i = 0; spec->arguments[i]; i++)
            g_ptr_array_add (got, spec->arguments[i]);
        }
      got_text = hb_joined (got);
      bro_test_same_string (failures, id, "argv", want, got_text);
    }
  if ((v = bro_json_value_member (expect, "workingDirectory")))
    {
      g_autofree char *want = hb_r (bro_json_value_get_string (v, ""));
      bro_test_same_string (failures, id, "workingDirectory", want,
                            launcher->started->len ? ((BroProcessSpec *) launcher->started->pdata[0])->working_directory : NULL);
    }
  if ((v = bro_json_value_member (expect, "progress")))
    {
      g_autoptr (GPtrArray) want = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *want_text = NULL, *got_text = hb_joined (sink->progress);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_ptr_array_add (want, g_strdup_printf ("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "/10000",
                                                bro_json_value_get_integer (bro_json_value_at (bro_json_value_at (v, i), 0), -1),
                                                bro_json_value_get_integer (bro_json_value_at (bro_json_value_at (v, i), 1), -1)));
      want_text = hb_joined (want);
      bro_test_same_string (failures, id, "progress", want_text, got_text);
    }
  if ((v = bro_json_value_member (expect, "raw")))
    {
      g_autofree char *want = hb_expected (v, FALSE), *got = hb_joined (sink->raw);
      bro_test_same_string (failures, id, "raw", want, got);
    }
  if ((v = bro_json_value_member (expect, "timer")))
    {
      BroTimerSchedule *t = clock->timers->len == 1 ? &g_array_index (clock->timers, BroTimerSchedule, 0) : NULL;
      if (!t || t->kind != BRO_TIMER_SCHEDULE_AT || t->instant.unix_milliseconds != FIRING_CLOCK_AT + bro_json_value_get_integer (v, 0) * 1000)
        bro_test_fail (failures, id, "timer");
    }
  else if (clock->timers->len)
    bro_test_fail (failures, id, "unexpected timer");
  return TRUE;
}

static void
test_handbrake (void)
{
  bro_test_run_cases ("adapters/handbrake.cases.json", handbrake_case);
}

/* The real HandBrakeCLI (where it and ffmpeg are installed): its presets, an encode of a generated clip, and the step's
 * timeout stopping a longer one. */
static void
test_handbrake_real (void)
{
  g_autofree char *cli = g_find_program_in_path ("HandBrakeCLI");
  g_autofree char *ffmpeg = g_find_program_in_path ("ffmpeg");
  g_autofree char *dir = NULL, *clip = NULL, *long_clip = NULL, *out = NULL, *out2 = NULL;
  g_autoptr (BroPlatformProcessLauncher) launcher = NULL;
  g_autoptr (TestFixedLocator) locator = NULL;
  g_autoptr (BroSystemClock) clock = NULL;
  g_autoptr (TestRecordingSink) sink = NULL;
  g_autoptr (BroHandBrake) handbrake = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroStepDefinition) step = NULL;
  g_autoptr (BroJsonValue) json = NULL;
  g_auto (GStrv) presets = NULL;
  BroHandBrakeRun run = { 0 };
  struct stat st;
  int status;
  if (!cli || !ffmpeg)
    {
      g_test_skip ("needs HandBrakeCLI and ffmpeg");
      return;
    }
  dir = g_build_filename (scratch, "handbrake-real", NULL);
  g_mkdir_with_parents (dir, 0755);
  clip = g_build_filename (dir, "clip.mkv", NULL);
  long_clip = g_build_filename (dir, "long.mkv", NULL);
  out = g_build_filename (dir, "clip-encoded.mkv", NULL);
  out2 = g_build_filename (dir, "long-encoded.mkv", NULL);
  {
    g_autofree char *cmd = g_strdup_printf ("'%s' -loglevel error -y -f lavfi -i testsrc=duration=3:size=320x240:rate=25 -c:v libx264 '%s' && "
                                            "'%s' -loglevel error -y -f lavfi -i testsrc=duration=600:size=1280x720:rate=25 -c:v libx264 -preset ultrafast '%s'",
                                            ffmpeg, clip, ffmpeg, long_clip);
    const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, NULL));
    g_assert_cmpint (status, ==, 0);
  }
  launcher = bro_platform_process_launcher_new ();
  locator = g_object_new (TEST_TYPE_FIXED_LOCATOR, NULL);
  locator->handbrake = g_strdup (cli);
  clock = bro_system_clock_new ();
  sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  handbrake = bro_hand_brake_new (BRO_PROCESS_LAUNCHER (launcher), BRO_TOOL_LOCATOR (locator), BRO_CLOCK (clock), g_get_home_dir ());

  presets = bro_hand_brake_presets (handbrake, NULL, &error);
  g_assert_null (error);
  g_assert_cmpuint (g_strv_length (presets), >, 40);
  g_assert_true (g_strv_contains ((const char *const *) presets, "Fast 1080p30"));

  json = bro_json_value_parse ("{\"id\":\"s1\",\"kind\":\"handbrake\",\"handbrake\":{\"preset\":\"Very Fast 480p30\"}}", -1);
  step = bro_step_definition_decode (json);
  g_assert_true (bro_hand_brake_encode (handbrake, step, clip, out, BRO_RUN_SINK (sink), NULL, &run, &error));
  g_assert_cmpint (run.exit.status, ==, 0);
  g_assert_false (run.timed_out);
  g_assert_cmpint (stat (out, &st), ==, 0);
  g_assert_cmpint (st.st_size, >, 1000);
  g_assert_cmpuint (sink->progress->len, >=, 1);
  g_assert_true (g_ptr_array_find_with_equal_func (sink->raw, "Encode done!", g_str_equal, NULL));

  /* A 10-minute 720p clip with a 2-second timeout: stopped, not finished. */
  step->timeout_seconds = 2;
  {
    gint64 t0 = g_get_monotonic_time ();
    g_assert_true (bro_hand_brake_encode (handbrake, step, long_clip, out2, BRO_RUN_SINK (sink), NULL, &run, &error));
    g_assert_true (run.timed_out);
    g_assert_cmpint (run.exit.status, !=, 0);
    g_assert_cmpfloat (since (t0), <, 20);
  }
  remove_tree (dir);
}
