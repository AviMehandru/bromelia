/* test-drives.inc: BroOsDriveState, BroDrivePoller and BroPlatformDeviceMonitor, included by test-adapters.c:
 * shared/fixtures/adapters/os-drive-states.cases.json, and the drive list on this machine (lsblk agrees). */

/* A clock whose timers fire when the test says so. */
#define TEST_TYPE_MANUAL_TIMER (test_manual_timer_get_type ())
G_DECLARE_FINAL_TYPE (TestManualTimer, test_manual_timer, TEST, MANUAL_TIMER, GObject)
struct _TestManualTimer {
  GObject parent_instance;
  BroTimerSchedule schedule;
  BroTimerFunc handler;
  gpointer data;
  GDestroyNotify destroy;
  gboolean cancelled;
};
static void test_manual_timer_iface_init (BroTimerHandleInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestManualTimer, test_manual_timer, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_TIMER_HANDLE, test_manual_timer_iface_init))

static void
mt_cancel (BroTimerHandle *t)
{
  TestManualTimer *self = TEST_MANUAL_TIMER (t);
  if (self->cancelled)
    return;
  self->cancelled = TRUE;
  if (self->destroy)
    self->destroy (self->data);
}

static void test_manual_timer_class_init (TestManualTimerClass *k) {}
static void test_manual_timer_init (TestManualTimer *self) {}
static void test_manual_timer_iface_init (BroTimerHandleInterface *iface) { iface->cancel = mt_cancel; }

#define TEST_TYPE_MANUAL_CLOCK (test_manual_clock_get_type ())
G_DECLARE_FINAL_TYPE (TestManualClock, test_manual_clock, TEST, MANUAL_CLOCK, GObject)
struct _TestManualClock {
  GObject parent_instance;
  GPtrArray *timers; /* TestManualTimer * */
};
static void test_manual_clock_iface_init (BroClockInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestManualClock, test_manual_clock, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, test_manual_clock_iface_init))

static BroTimerHandle *
mc_timer (BroClock *c, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy)
{
  TestManualTimer *t = g_object_new (TEST_TYPE_MANUAL_TIMER, NULL);
  t->schedule = *schedule;
  t->handler = handler;
  t->data = data;
  t->destroy = destroy;
  g_ptr_array_add (TEST_MANUAL_CLOCK (c)->timers, g_object_ref (t));
  return BRO_TIMER_HANDLE (t);
}

/* Fires every timer that isn't cancelled. */
static void
manual_clock_tick (TestManualClock *self)
{
  for (guint i = 0; i < self->timers->len; i++)
    {
      TestManualTimer *t = self->timers->pdata[i];
      if (!t->cancelled)
        t->handler (t->data);
    }
}

static void test_manual_clock_finalize (GObject *o) { g_ptr_array_unref (TEST_MANUAL_CLOCK (o)->timers); G_OBJECT_CLASS (test_manual_clock_parent_class)->finalize (o); }
static void test_manual_clock_class_init (TestManualClockClass *k) { G_OBJECT_CLASS (k)->finalize = test_manual_clock_finalize; }
static void test_manual_clock_init (TestManualClock *self) { self->timers = g_ptr_array_new_with_free_func (g_object_unref); }

static void
test_manual_clock_iface_init (BroClockInterface *iface)
{
  iface->now = firing_now;
  iface->monotonic = firing_monotonic;
  iface->sleep = firing_sleep;
  iface->timer = mc_timer;
}

static char *
event_text (const BroDeviceEvent *e)
{
  switch (e->kind)
    {
    case BRO_DEVICE_EVENT_DRIVE_APPEARED: return g_strdup_printf ("driveAppeared %s %s", e->drive->device, e->drive->identification);
    case BRO_DEVICE_EVENT_DRIVE_VANISHED: return g_strdup_printf ("driveVanished %s", e->device);
    case BRO_DEVICE_EVENT_MEDIA_ARRIVED: return g_strdup_printf ("mediaArrived %s", e->device);
    case BRO_DEVICE_EVENT_MEDIA_REMOVED: return g_strdup_printf ("mediaRemoved %s", e->device);
    case BRO_DEVICE_EVENT_TRAY_OPENED: return g_strdup_printf ("trayOpened %s", e->device);
    case BRO_DEVICE_EVENT_MOUNTED: return g_strdup_printf ("mounted %s %s", e->device, e->path);
    case BRO_DEVICE_EVENT_UNMOUNTED: return g_strdup_printf ("unmounted %s", e->device);
    }
  return g_strdup ("?");
}

static GPtrArray *
states_from (BroJsonValue *list)
{
  GPtrArray *states = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_state_free);
  for (guint i = 0; i < bro_json_value_length (list); i++)
    {
      BroJsonValue *j = bro_json_value_at (list, i);
      g_ptr_array_add (states, bro_os_drive_state_new (bro_os_drive_new (bro_json_value_get_string (bro_json_value_member (j, "device"), ""),
                                                                         bro_json_value_get_string (bro_json_value_member (j, "identification"), ""),
                                                                         bro_json_value_get_string (bro_json_value_member (j, "mountPath"), NULL)),
                                                       bro_json_value_get_bool (bro_json_value_member (j, "media"), FALSE)));
    }
  return states;
}

typedef struct {
  BroJsonValue *snapshots;
  guint index;
} Script;

static GPtrArray *
scripted_snapshot (gpointer data)
{
  Script *s = data;
  return states_from (bro_json_value_at (s->snapshots, s->index));
}

static void
collect (const BroDeviceEvent *e, gpointer data)
{
  g_ptr_array_add (data, event_text (e));
}

static gboolean
drive_states_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BroJsonValue *snapshots = bro_json_value_member (given, "snapshots");
  if (snapshots)
    {
      g_autoptr (TestManualClock) clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
      g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
      Script script = { snapshots, 0 };
      g_autoptr (BroDrivePoller) poller = bro_drive_poller_new (scripted_snapshot, &script, NULL, BRO_CLOCK (clock),
                                                                (BroDuration) { bro_json_value_get_number (bro_json_value_member (given, "interval"), 0) });
      BroJsonValue *ticks = bro_json_value_member (expect, "afterTicks");
      TestManualTimer *timer;
      bro_drive_poller_start (poller, collect, got, NULL);
      if (clock->timers->len != 1)
        {
          bro_test_fail (failures, id, "%u timers", clock->timers->len);
          return TRUE;
        }
      timer = clock->timers->pdata[0];
      if (timer->schedule.kind != BRO_TIMER_SCHEDULE_EVERY
          || timer->schedule.interval.seconds != bro_json_value_get_number (bro_json_value_member (expect, "timer"), -1))
        bro_test_fail (failures, id, "schedule");
      for (guint i = 0; i < bro_json_value_length (ticks); i++)
        {
          g_autofree char *want = hb_expected (bro_json_value_at (ticks, i), FALSE), *got_text = NULL;
          script.index++;
          g_ptr_array_set_size (got, 0);
          manual_clock_tick (clock);
          got_text = hb_joined (got);
          bro_test_same_string (failures, id, "events", want, got_text);
        }
      {
        g_autoptr (GPtrArray) drives = bro_drive_poller_current_drives (poller);
        g_autoptr (GPtrArray) states = scripted_snapshot (&script);
        if (drives->len != states->len)
          bro_test_fail (failures, id, "currentDrives");
      }
      bro_drive_poller_stop (poller);
      if (!timer->cancelled)
        bro_test_fail (failures, id, "not stopped");
      return TRUE;
    }
  {
    g_autoptr (GPtrArray) before = states_from (bro_json_value_member (given, "before"));
    g_autoptr (GPtrArray) after = states_from (bro_json_value_member (given, "after"));
    g_autoptr (GPtrArray) events = bro_os_drive_state_changes (before, after);
    g_autoptr (GPtrArray) texts = g_ptr_array_new_with_free_func (g_free);
    g_autofree char *want = hb_expected (bro_json_value_member (expect, "events"), FALSE), *got = NULL;
    for (guint i = 0; i < events->len; i++)
      g_ptr_array_add (texts, event_text (events->pdata[i]));
    got = hb_joined (texts);
    bro_test_same_string (failures, id, "events", want, got);
  }
  return TRUE;
}

static void
test_drive_states (void)
{
  bro_test_run_cases ("adapters/os-drive-states.cases.json", drive_states_case);
}

/* The optical drives here, as lsblk (udev) lists them: the same devices and vendors (the model is the drive's own
 * INQUIRY product, as on Windows and in MakeMKV's drive list; udev's may be a friendlier name); nothing reported while
 * nothing changes. */
static void
test_platform_device_monitor (void)
{
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroPlatformDeviceMonitor) monitor = bro_platform_device_monitor_new (BRO_CLOCK (clock), 0.2);
  g_autoptr (GPtrArray) states = bro_platform_device_monitor_snapshot (monitor);
  g_autoptr (GPtrArray) events = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *lsblk = g_find_program_in_path ("lsblk");
  for (guint i = 0; i < states->len; i++)
    {
      BroOsDriveState *s = states->pdata[i];
      g_test_message ("%s: %s, media %d, mounted at %s", s->drive->device, s->drive->identification, s->media, s->drive->mount_path);
      g_assert_true (g_str_has_prefix (s->drive->device, "/dev/sr"));
    }
  if (lsblk)
    {
      const char *argv[] = { lsblk, "-dnro", "NAME,TYPE,VENDOR,MODEL", NULL };
      g_autofree char *out = NULL;
      g_auto (GStrv) lines = NULL;
      g_autoptr (GPtrArray) want = g_ptr_array_new_with_free_func (g_free), got = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *want_text = NULL, *got_text = NULL;
      int status;
      g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, &status, NULL));
      lines = g_strsplit (out, "\n", -1);
      for (guint i = 0; lines[i]; i++)
        {
          g_auto (GStrv) f = g_strsplit (lines[i], " ", 4);
          if (g_strv_length (f) == 4 && g_str_equal (f[1], "rom"))
            {
              /* lsblk escapes spaces as \x20 */
              g_autoptr (GString) vendor = g_string_new (f[2]);
              g_string_replace (vendor, "\\x20", " ", 0);
              g_ptr_array_add (want, g_strdup_printf ("/dev/%s %s", f[0], g_strstrip (vendor->str)));
            }
        }
      for (guint i = 0; i < states->len; i++)
        {
          BroOsDriveState *s = states->pdata[i];
          g_auto (GStrv) words = g_strsplit (s->drive->identification, " ", 2);
          g_ptr_array_add (got, g_strdup_printf ("%s %s", s->drive->device, words[0] ? words[0] : ""));
        }
      want_text = hb_joined (want);
      got_text = hb_joined (got);
      g_assert_cmpstr (got_text, ==, want_text);
    }
  bro_platform_device_monitor_start (monitor, collect, events, NULL);
  g_usleep (G_USEC_PER_SEC);
  bro_platform_device_monitor_stop (monitor);
  g_assert_cmpuint (events->len, ==, 0);
}
