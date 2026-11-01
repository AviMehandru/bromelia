/* bro-test-kit.c */
#include "bro-test-kit.h"
#include "bro-conflict-namer.h"
#include "bro-moved-item.h"

#include "bro-byte-stream.h"
#include "bro-directory-entry.h"
#include "bro-file-info.h"
#include "bro-timer-handle.h"
#include "bro-volume-info.h"

#include <glib/gstdio.h>
#include <string.h>

/* What a launcher's processes count, shared with them (they may outlive it). */
struct _TestLaunchCounts {
  gatomicrefcount refs;
  gint held;
  gint running;
};

static struct _TestLaunchCounts *
counts_ref (struct _TestLaunchCounts *c)
{
  g_atomic_ref_count_inc (&c->refs);
  return c;
}

static void
counts_unref (struct _TestLaunchCounts *c)
{
  if (c && g_atomic_ref_count_dec (&c->refs))
    g_free (c);
}

void
bro_test_remove_tree (const char *path)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const char *name;
  if (dir)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, name, NULL);
          bro_test_remove_tree (child);
        }
      g_dir_close (dir);
      g_rmdir (path);
    }
  else
    g_unlink (path);
}

static gboolean
script_stopped (TestScript *self)
{
  gboolean stopped;
  g_mutex_lock (&self->lock);
  stopped = self->stopped;
  g_mutex_unlock (&self->lock);
  return stopped;
}

/* Writes @text to @path, through the launcher's file system when it has one (the test root's paths use '/'). */
static void
script_write (TestScript *self, const char *path, const char *text)
{
  g_autofree char *dir = g_path_get_dirname (path);
  if (self->files)
    {
      g_autoptr (GBytes) bytes = g_bytes_new (text, strlen (text));
      bro_file_system_create_directory (self->files, dir, FALSE, NULL);
      bro_file_system_write_atomically (self->files, path, bytes, 0644, NULL);
      return;
    }
  g_mkdir_with_parents (dir, 0755);
  g_file_set_contents (path, text, -1, NULL);
}

/* The process ended (once): the launcher counts one fewer running. */
static void
script_ended (TestScript *self)
{
  gboolean first;
  g_mutex_lock (&self->lock);
  first = !self->ended;
  self->ended = TRUE;
  g_mutex_unlock (&self->lock);
  if (first && self->counts)
    g_atomic_int_dec_and_test (&self->counts->running);
}

static void test_script_iface_init (BroRunningProcessInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestScript, test_script, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_RUNNING_PROCESS, test_script_iface_init))

static BroOutputLine *
script_lines (BroRunningProcess *p)
{
  TestScript *self = TEST_SCRIPT (p);
  BroOutputLine *line;
  if (!script_stopped (self) && self->handed > 0 && self->handed == self->cancel_after && self->cancel)
    bro_cancellation_source_cancel (self->cancel);
  if (!script_stopped (self) && self->handed > 0 && self->handed == self->delete_after && self->delete_dir)
    bro_test_remove_tree (self->delete_dir);
  if (!script_stopped (self) && self->lines && self->lines[self->handed])
    {
      line = bro_output_line_new ();
      line->stream = BRO_OUTPUT_SOURCE_STDOUT;
      for (guint i = 0; self->stderr_at && i < self->stderr_at->len; i++)
        if (g_array_index (self->stderr_at, int, i) == self->handed)
          line->stream = BRO_OUTPUT_SOURCE_STDERR;
      line->text = g_strdup (self->lines[self->handed++]);
      return line;
    }
  if (self->holds && !script_stopped (self) && !self->ended)
    {
      /* A rip in progress: it runs until it is stopped, then takes stop_takes to end. */
      g_atomic_int_inc (&self->counts->held);
      g_mutex_lock (&self->lock);
      while (!self->stopped)
        g_cond_wait (&self->stop_cond, &self->lock);
      g_mutex_unlock (&self->lock);
      if (self->stop_takes > 0)
        g_usleep ((gulong) (self->stop_takes * G_USEC_PER_SEC));
      g_atomic_int_dec_and_test (&self->counts->held);
    }
  else if (script_stopped (self) && !self->ended && self->stop_takes > 0)
    g_usleep ((gulong) (self->stop_takes * G_USEC_PER_SEC));
  if (!script_stopped (self) && self->stalls)
    self->exit = (BroProcessExit) { -1, 15, self->stall_seconds, FALSE, FALSE };
  else if (script_stopped (self))
    self->exit = (BroProcessExit) { -1, 15, -1, FALSE, self->reason == BRO_STOP_REASON_CANCELLED || self->reason == BRO_STOP_REASON_SHUTDOWN };
  else
    {
      static const char *const one_title[] = { "title_t00.mkv", NULL };
      for (const char *const *name = self->write_files ? (const char *const *) self->write_files : one_title; self->write_file_in && *name; name++)
        {
          g_autofree char *f = !**name       ? g_strdup (self->write_file_in)
                               : self->files ? g_strconcat (self->write_file_in, "/", *name, NULL)
                                             : g_build_filename (self->write_file_in, *name, NULL);
          script_write (self, f, "");
        }
      for (int i = 0; self->split_parts > 0 && self->arguments && self->arguments[i]; i++)
        if (g_str_equal (self->arguments[i], "-o") && self->arguments[i + 1])
          {
            for (int k = 1; k <= self->split_parts; k++)
              {
                g_autoptr (GString) part = g_string_new (self->arguments[i + 1]);
                g_autofree char *number = g_strdup_printf ("%03d", k);
                g_string_replace (part, "%03d", number, 0);
                script_write (self, part->str, "");
              }
            self->split_parts = 0;
            break;
          }
      if (self->write_index >= 0 && self->arguments && (int) g_strv_length (self->arguments) > self->write_index)
        {
          script_write (self, self->arguments[self->write_index], self->write_text);
          self->write_index = -1;
        }
      self->exit = bro_process_exit_status (self->exit_code);
    }
  script_ended (self);
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
  g_mutex_lock (&self->lock);
  if (!self->stopped)
    {
      self->stopped = TRUE;
      self->reason = reason;
    }
  g_cond_broadcast (&self->stop_cond);
  g_mutex_unlock (&self->lock);
}

static BroBroMessage *
script_transcript_problem (BroRunningProcess *p)
{
  TestScript *self = TEST_SCRIPT (p);
  BroJsonValue *params;
  if (!self->transcript_fails_at)
    return NULL;
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (self->transcript_fails_at));
  bro_json_value_set (params, "reason", bro_json_value_new_string ("No space left on device"));
  return bro_bro_message_new (BRO_MSG_PROCESS_NO_TRANSCRIPT, params, BRO_SEVERITY_WARNING);
}

static void
test_script_finalize (GObject *o)
{
  TestScript *self = TEST_SCRIPT (o);
  g_strfreev (self->lines);
  g_clear_pointer (&self->stderr_at, g_array_unref);
  g_strfreev (self->write_files);
  g_strfreev (self->arguments);
  g_free (self->write_file_in);
  g_free (self->delete_dir);
  g_free (self->transcript_fails_at);
  g_free (self->write_text);
  script_ended (self);
  counts_unref (self->counts);
  g_clear_object (&self->files);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->stop_cond);
  G_OBJECT_CLASS (test_script_parent_class)->finalize (o);
}

static void test_script_class_init (TestScriptClass *k) { G_OBJECT_CLASS (k)->finalize = test_script_finalize; }

static void
test_script_init (TestScript *self)
{
  self->cancel_after = -1;
  self->delete_after = -1;
  self->write_index = -1;
  g_mutex_init (&self->lock);
  g_cond_init (&self->stop_cond);
}

static void
test_script_iface_init (BroRunningProcessInterface *iface)
{
  iface->lines = script_lines;
  iface->wait = script_wait;
  iface->stop = script_stop;
  iface->transcript_problem = script_transcript_problem;
}

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
  GStrv lines = self->lines_for ? self->lines_for (spec, self->lines_for_data) : NULL;
  script->lines = lines ? lines : g_strdupv (self->lines);
  script->holds = self->runs_until_stopped && self->runs_until_stopped (spec, self->runs_until_stopped_data);
  script->stop_takes = self->stop_takes;
  script->files = self->files ? g_object_ref (self->files) : NULL;
  script->counts = counts_ref (self->counts);
  g_atomic_int_inc (&self->counts->running);
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->started, spec_copy (spec));
  g_mutex_unlock (&self->lock);
  script->stderr_at = self->stderr_at ? g_array_ref (self->stderr_at) : NULL;
  script->write_files = g_strdupv (self->write_files);
  script->stalls = self->stalls;
  script->stall_seconds = spec->has_stall_timeout ? spec->stall_timeout.seconds : 0;
  script->exit_code = self->exit_code;
  script->write_file_in = g_strdup (self->write_file_in);
  script->cancel_after = self->cancel_after;
  script->delete_after = self->delete_after;
  script->delete_dir = g_strdup (self->delete_dir);
  script->cancel = self->cancel;
  script->arguments = g_strdupv (spec->arguments);
  script->split_parts = self->split_parts;
  script->write_index = self->write_index;
  script->write_text = g_strdup (self->write_text);
  script->transcript_fails_at = self->transcript_fails ? g_strdup (spec->transcript ? spec->transcript : "") : NULL;
  g_mutex_lock (&self->lock);
  g_set_object (&self->last, script);
  g_mutex_unlock (&self->lock);
  return BRO_RUNNING_PROCESS (script);
}

int
test_scripted_launcher_held (TestScriptedLauncher *self)
{
  return g_atomic_int_get (&self->counts->held);
}

int
test_scripted_launcher_running (TestScriptedLauncher *self)
{
  return g_atomic_int_get (&self->counts->running);
}

GPtrArray *
test_scripted_launcher_specs (TestScriptedLauncher *self)
{
  GPtrArray *specs = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_process_spec_free);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->started->len; i++)
    g_ptr_array_add (specs, spec_copy (g_ptr_array_index (self->started, i)));
  g_mutex_unlock (&self->lock);
  return specs;
}

static void
test_scripted_launcher_finalize (GObject *o)
{
  TestScriptedLauncher *self = TEST_SCRIPTED_LAUNCHER (o);
  g_strfreev (self->lines);
  g_clear_pointer (&self->stderr_at, g_array_unref);
  g_strfreev (self->write_files);
  g_free (self->write_file_in);
  g_free (self->delete_dir);
  g_free (self->write_text);
  g_ptr_array_unref (self->started);
  g_clear_object (&self->last);
  g_clear_object (&self->files);
  counts_unref (self->counts);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_scripted_launcher_parent_class)->finalize (o);
}

static void test_scripted_launcher_class_init (TestScriptedLauncherClass *k) { G_OBJECT_CLASS (k)->finalize = test_scripted_launcher_finalize; }

static void
test_scripted_launcher_init (TestScriptedLauncher *self)
{
  self->started = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_process_spec_free);
  self->counts = g_new0 (struct _TestLaunchCounts, 1);
  g_atomic_ref_count_init (&self->counts->refs);
  g_mutex_init (&self->lock);
  self->cancel_after = -1;
  self->delete_after = -1;
  self->write_index = -1;
}

static void test_scripted_launcher_iface_init (BroProcessLauncherInterface *iface) { iface->start = launcher_start; }

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
static BroBroMessage *
rl_warning (BroIsolationLease *l)
{
  return TEST_RECORDING_LEASE (l)->owner->warns ? bro_bro_message_new (BRO_MSG_MAKEMKV_GUI_RUNNING, NULL, BRO_SEVERITY_WARNING) : NULL;
}

/* Adds @line (taken) to the isolation's log. */
static void
ri_log (TestRecordingIsolation *self, char *line)
{
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->log, line);
  g_mutex_unlock (&self->lock);
}

static void rl_first (BroIsolationLease *l) { ri_log (TEST_RECORDING_LEASE (l)->owner, g_strdup ("firstOutput")); }
static BroBroMessage *
rl_release (BroIsolationLease *l)
{
  TestRecordingIsolation *owner = TEST_RECORDING_LEASE (l)->owner;
  BroJsonValue *params;
  ri_log (owner, g_strdup ("release"));
  if (!owner->release_problem)
    return NULL;
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string ("<work>/.MakeMKV/settings.conf"));
  bro_json_value_set (params, "reason", bro_json_value_new_string (owner->release_problem));
  return bro_bro_message_new (BRO_MSG_MAKEMKV_KEY_NOT_REMOVED, params, BRO_SEVERITY_WARNING);
}

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
  iface->warning = rl_warning;
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
  ri_log (self, g_strdup ("prepare"));
  lease->owner = g_object_ref (self);
  lease->home = g_strdup (settings->work_directory);
  lease->profile = settings->profile_xml ? g_strconcat (settings->work_directory, "/profile.mmcp.xml", NULL) : NULL;
  return BRO_ISOLATION_LEASE (lease);
}

static GPtrArray *
ri_recover (BroSettingsIsolation *i, GPtrArray *work_directories)
{
  TestRecordingIsolation *self = TEST_RECORDING_ISOLATION (i);
  GString *line = g_string_new ("recover");
  for (guint n = 0; n < work_directories->len; n++)
    g_string_append_printf (line, " %s", (const char *) g_ptr_array_index (work_directories, n));
  ri_log (self, g_string_free (line, FALSE));
  return g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
}

static void
test_recording_isolation_finalize (GObject *o)
{
  g_ptr_array_unref (TEST_RECORDING_ISOLATION (o)->log);
  g_free (TEST_RECORDING_ISOLATION (o)->release_problem);
  g_mutex_clear (&TEST_RECORDING_ISOLATION (o)->lock);
  G_OBJECT_CLASS (test_recording_isolation_parent_class)->finalize (o);
}

static void test_recording_isolation_class_init (TestRecordingIsolationClass *k) { G_OBJECT_CLASS (k)->finalize = test_recording_isolation_finalize; }
static void
test_recording_isolation_init (TestRecordingIsolation *self)
{
  self->log = g_ptr_array_new_with_free_func (g_free);
  g_mutex_init (&self->lock);
}
static void
test_recording_isolation_iface_init (BroSettingsIsolationInterface *iface)
{
  iface->prepare = ri_prepare;
  iface->recover = ri_recover;
}

static void test_fixed_locator_iface_init (BroToolLocatorInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFixedLocator, test_fixed_locator, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_TOOL_LOCATOR, test_fixed_locator_iface_init))

static BroToolInfo *
fl_locate (BroToolLocator *l, BroToolKind tool)
{
  TestFixedLocator *self = TEST_FIXED_LOCATOR (l);
  BroToolInfo *info = bro_tool_info_new ();
  info->tool = tool;
  if (tool == BRO_TOOL_KIND_MAKEMKVCON && self->path)
    {
      info->path = g_strdup (self->path);
      info->version = g_strdup (self->makemkvcon_version);
    }
  else if (tool == BRO_TOOL_KIND_APPRISE && self->apprise)
    info->path = g_strdup (self->apprise);
  else if (tool == BRO_TOOL_KIND_MKVMERGE && self->mkvmerge)
    info->path = g_strdup (self->mkvmerge);
  else if (tool == BRO_TOOL_KIND_MKVEXTRACT && self->mkvextract)
    info->path = g_strdup (self->mkvextract);
  else if (tool == BRO_TOOL_KIND_HANDBRAKE && self->handbrake)
    info->path = g_strdup (self->handbrake);
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
  g_free (TEST_FIXED_LOCATOR (o)->mkvmerge);
  g_free (TEST_FIXED_LOCATOR (o)->mkvextract);
  g_free (TEST_FIXED_LOCATOR (o)->handbrake);
  g_free (TEST_FIXED_LOCATOR (o)->makemkvcon_version);
  G_OBJECT_CLASS (test_fixed_locator_parent_class)->finalize (o);
}
static void test_fixed_locator_class_init (TestFixedLocatorClass *k) { G_OBJECT_CLASS (k)->finalize = test_fixed_locator_finalize; }
static void test_fixed_locator_init (TestFixedLocator *self) {}
static void test_fixed_locator_iface_init (BroToolLocatorInterface *iface) { iface->locate = fl_locate; }
/* ---- a clock that moves only when told (the scenarios' fake clock) ------------------------------------------- */

typedef struct {
  gatomicrefcount refs;
  gint64 due_ms;
  gint64 interval_ms;
  guint64 order;
  gboolean cancelled; /* under the clock's lock */
  BroTimerFunc handler;
  gpointer data;
  GDestroyNotify destroy;
} Pending;

static Pending *
pending_ref (Pending *p)
{
  g_atomic_ref_count_inc (&p->refs);
  return p;
}

static void
pending_unref (Pending *p)
{
  if (!g_atomic_ref_count_dec (&p->refs))
    return;
  if (p->destroy)
    p->destroy (p->data);
  g_free (p);
}

#define TEST_TYPE_MANUAL_TIMER (test_manual_timer_get_type ())
G_DECLARE_FINAL_TYPE (TestManualTimer, test_manual_timer, TEST, MANUAL_TIMER, GObject)

struct _TestManualTimer {
  GObject parent_instance;
  TestManualClock *clock; /* a reference */
  Pending *pending;       /* a reference */
};

static void test_manual_timer_iface_init (BroTimerHandleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestManualTimer, test_manual_timer, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_TIMER_HANDLE, test_manual_timer_iface_init))

static void
manual_timer_cancel (BroTimerHandle *handle)
{
  TestManualTimer *t = TEST_MANUAL_TIMER (handle);
  g_mutex_lock (&t->clock->lock);
  t->pending->cancelled = TRUE;
  g_mutex_unlock (&t->clock->lock);
}

static void
test_manual_timer_iface_init (BroTimerHandleInterface *iface)
{
  iface->cancel = manual_timer_cancel;
}

static void
test_manual_timer_finalize (GObject *object)
{
  TestManualTimer *t = TEST_MANUAL_TIMER (object);
  pending_unref (t->pending);
  g_object_unref (t->clock);
  G_OBJECT_CLASS (test_manual_timer_parent_class)->finalize (object);
}

static void
test_manual_timer_class_init (TestManualTimerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_manual_timer_finalize;
}

static void
test_manual_timer_init (TestManualTimer *self)
{
}

static void test_manual_clock_iface_init (BroClockInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestManualClock, test_manual_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, test_manual_clock_iface_init))

static BroInstant
manual_now (BroClock *clock)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  BroInstant at;
  g_mutex_lock (&self->lock);
  at.unix_milliseconds = self->now_ms;
  g_mutex_unlock (&self->lock);
  return at;
}

static BroDuration
manual_monotonic (BroClock *clock)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  BroDuration d;
  g_mutex_lock (&self->lock);
  d.seconds = (double) (self->now_ms - TEST_MANUAL_CLOCK_START_MS) / 1000;
  g_mutex_unlock (&self->lock);
  return d;
}

static gboolean
manual_sleep (BroClock *clock, BroDuration duration, BroCancellationToken *cancel, BroBroError **error)
{
  g_assert_not_reached ();
}

static BroTimerHandle *
manual_timer (BroClock *clock, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  TestManualTimer *t = g_object_new (TEST_TYPE_MANUAL_TIMER, NULL);
  Pending *p = g_new0 (Pending, 1);
  g_atomic_ref_count_init (&p->refs);
  p->handler = handler;
  p->data = data;
  p->destroy = destroy;
  g_mutex_lock (&self->lock);
  p->order = self->made++;
  if (schedule->kind == BRO_TIMER_SCHEDULE_AT)
    p->due_ms = schedule->instant.unix_milliseconds;
  else
    {
      p->interval_ms = MAX (1, (gint64) (schedule->interval.seconds * 1000 + 0.5));
      p->due_ms = self->now_ms + p->interval_ms;
    }
  g_ptr_array_add (self->timers, pending_ref (p));
  g_mutex_unlock (&self->lock);
  t->clock = g_object_ref (self);
  t->pending = p;
  return BRO_TIMER_HANDLE (t);
}

static BroLocalTime
manual_local_time (BroClock *clock, BroInstant instant)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  gint64 seconds = instant.unix_milliseconds / 1000 - (instant.unix_milliseconds % 1000 < 0);
  g_autoptr (GDateTime) t = NULL;
  g_mutex_lock (&self->lock);
  seconds += (gint64) self->utc_offset_minutes * 60;
  g_mutex_unlock (&self->lock);
  t = g_date_time_new_from_unix_utc (seconds);
  return (BroLocalTime) { g_date_time_get_year (t), g_date_time_get_month (t), g_date_time_get_day_of_month (t),
                          g_date_time_get_hour (t), g_date_time_get_minute (t), g_date_time_get_second (t) };
}

static void
test_manual_clock_iface_init (BroClockInterface *iface)
{
  iface->now = manual_now;
  iface->monotonic = manual_monotonic;
  iface->sleep = manual_sleep;
  iface->timer = manual_timer;
  iface->local_time = manual_local_time;
}

static void
test_manual_clock_finalize (GObject *object)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (object);
  g_ptr_array_unref (self->timers);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_manual_clock_parent_class)->finalize (object);
}

static void
test_manual_clock_class_init (TestManualClockClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_manual_clock_finalize;
}

static void
test_manual_clock_init (TestManualClock *self)
{
  g_mutex_init (&self->lock);
  self->now_ms = TEST_MANUAL_CLOCK_START_MS;
  self->timers = g_ptr_array_new_with_free_func ((GDestroyNotify) pending_unref);
}

void
test_manual_clock_advance (TestManualClock *self, gint64 ms, void (*settle) (gpointer), gpointer settle_data)
{
  gint64 target;
  g_mutex_lock (&self->lock);
  target = self->now_ms + ms;
  g_mutex_unlock (&self->lock);
  for (;;)
    {
      Pending *next = NULL;
      g_mutex_lock (&self->lock);
      for (guint i = self->timers->len; i > 0; i--)
        if (((Pending *) g_ptr_array_index (self->timers, i - 1))->cancelled)
          g_ptr_array_remove_index (self->timers, i - 1);
      for (guint i = 0; i < self->timers->len; i++)
        {
          Pending *p = g_ptr_array_index (self->timers, i);
          if (p->due_ms <= target && (!next || p->due_ms < next->due_ms || (p->due_ms == next->due_ms && p->order < next->order)))
            next = p;
        }
      if (!next)
        {
          self->now_ms = target;
          g_mutex_unlock (&self->lock);
          return;
        }
      pending_ref (next);
      self->now_ms = MAX (self->now_ms, next->due_ms);
      if (next->interval_ms > 0)
        next->due_ms += next->interval_ms;
      else
        g_ptr_array_remove (self->timers, next);
      g_mutex_unlock (&self->lock);
      next->handler (next->data);
      pending_unref (next);
      if (settle)
        settle (settle_data);
    }
}


gboolean
test_manual_clock_next_due (TestManualClock *self, BroInstant *due)
{
  gboolean found = FALSE;
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->timers->len; i++)
    {
      Pending *p = g_ptr_array_index (self->timers, i);
      if (!p->cancelled && (!found || p->due_ms < due->unix_milliseconds))
        {
          found = TRUE;
          due->unix_milliseconds = p->due_ms;
        }
    }
  g_mutex_unlock (&self->lock);
  return found;
}

void
test_manual_clock_cancel_all (TestManualClock *self)
{
  g_autoptr (GPtrArray) dropped = NULL;
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->timers->len; i++)
    ((Pending *) g_ptr_array_index (self->timers, i))->cancelled = TRUE;
  dropped = g_steal_pointer (&self->timers);
  self->timers = g_ptr_array_new_with_free_func ((GDestroyNotify) pending_unref);
  g_mutex_unlock (&self->lock);
}

int
test_manual_clock_pending (TestManualClock *self)
{
  int n = 0;
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->timers->len; i++)
    n += !((Pending *) g_ptr_array_index (self->timers, i))->cancelled;
  g_mutex_unlock (&self->lock);
  return n;
}

/* ---- files and folders in memory ------------------------------------------------------------------------------- */

static void memory_fs_iface_init (BroFileSystemInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestMemoryFs, test_memory_fs, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, memory_fs_iface_init))

/* FALSE (and @error) when a test made @op on @path fail. */
/* FALSE (and @error: fs.failed, reason "frozen") while the files are frozen. */
static gboolean
fs_not_frozen (TestMemoryFs *self, const char *op, const char *path, BroBroError **error)
{
  BroJsonValue *params;
  if (!g_atomic_int_get (&self->frozen))
    return TRUE;
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string (op));
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string ("frozen"));
  bro_bro_error_set (error, "fs.failed", params);
  return FALSE;
}

static gboolean
fs_check (TestMemoryFs *self, const char *op, const char *path, BroBroError **error)
{
  g_autofree char *key = g_strconcat (op, "\n", path, NULL);
  const BroBroError *found = NULL;
  if ((strcmp (op, "write") == 0 || strcmp (op, "rename") == 0 || strcmp (op, "remove") == 0 || strcmp (op, "sync") == 0
       || strcmp (op, "syncDirectory") == 0)
      && !fs_not_frozen (self, op, path, error))
    return FALSE;
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->fails->len && !found; i++)
    if (strcmp (g_ptr_array_index (self->fails, i), key) == 0)
      found = g_ptr_array_index (self->fail_errors, i);
  if (found && error)
    *error = bro_bro_error_copy (found);
  g_mutex_unlock (&self->lock);
  return found == NULL;
}

static void
not_found (BroBroError **error, const char *path)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_bro_error_set (error, "fs.notFound", params);
}

static char *
parent_of (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash && slash > path ? g_strndup (path, slash - path) : g_strdup ("/");
}

/* Every folder down to @path; the caller holds the lock. */
static void
put_folder_locked (TestMemoryFs *self, const char *path)
{
  g_auto (GStrv) parts = g_strsplit (path, "/", -1);
  g_autoptr (GString) current = g_string_new ("");
  for (guint i = 0; parts[i]; i++)
    if (*parts[i])
      {
        g_string_append_printf (current, "/%s", parts[i]);
        g_hash_table_add (self->folders, g_strdup (current->str));
      }
}

void
test_memory_fs_put (TestMemoryFs *self, const char *path, const void *bytes, gsize length)
{
  g_autofree char *folder = parent_of (path);
  g_mutex_lock (&self->lock);
  put_folder_locked (self, folder);
  g_hash_table_replace (self->files, g_strdup (path), g_bytes_new (bytes, length));
  g_mutex_unlock (&self->lock);
}

void
test_memory_fs_put_folder (TestMemoryFs *self, const char *path)
{
  g_mutex_lock (&self->lock);
  put_folder_locked (self, path);
  g_mutex_unlock (&self->lock);
}

void
test_memory_fs_freeze (TestMemoryFs *self)
{
  g_atomic_int_set (&self->frozen, TRUE);
}

void
test_memory_fs_thaw (TestMemoryFs *self)
{
  g_atomic_int_set (&self->frozen, FALSE);
}

gboolean
test_memory_fs_frozen (TestMemoryFs *self)
{
  return g_atomic_int_get (&self->frozen);
}

void
test_memory_fs_fail (TestMemoryFs *self, const char *op, const char *path, const BroBroError *error)
{
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->fails, g_strconcat (op, "\n", path, NULL));
  g_ptr_array_add (self->fail_errors, bro_bro_error_copy (error));
  g_mutex_unlock (&self->lock);
}

void
test_memory_fs_unfail (TestMemoryFs *self)
{
  g_mutex_lock (&self->lock);
  g_ptr_array_set_size (self->fails, 0);
  g_ptr_array_set_size (self->fail_errors, 0);
  g_mutex_unlock (&self->lock);
}

void
test_memory_fs_fail_reads (TestMemoryFs *self, const char *path, const BroBroError *error)
{
  g_mutex_lock (&self->lock);
  g_hash_table_replace (self->read_fails, g_strdup (path), bro_bro_error_copy (error));
  g_mutex_unlock (&self->lock);
}

static gboolean
fs_exists (BroFileSystem *fs, const char *path)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  gboolean found;
  g_mutex_lock (&self->lock);
  found = g_hash_table_contains (self->files, path) || g_hash_table_contains (self->folders, path);
  g_mutex_unlock (&self->lock);
  return found;
}

static GBytes *
fs_read (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  const BroBroError *fails;
  GBytes *b = NULL;
  if (self->on_read)
    self->on_read (path, self->on_read_data);
  g_mutex_lock (&self->lock);
  fails = g_hash_table_lookup (self->read_fails, path);
  if (fails && error)
    *error = bro_bro_error_copy (fails);
  if (!fails)
    {
      b = g_hash_table_lookup (self->files, path);
      b = b ? g_bytes_ref (b) : NULL;
    }
  g_mutex_unlock (&self->lock);
  if (!b && !fails)
    bro_bro_error_set (error, "fs.notFound", NULL);
  return b;
}

static BroFileInfo *
fs_stat (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  BroFileInfo *info = NULL;
  GBytes *b;
  g_mutex_lock (&self->lock);
  b = g_hash_table_lookup (self->files, path);
  if (b || g_hash_table_contains (self->folders, path))
    {
      info = bro_file_info_new ();
      info->size = b ? (gint64) g_bytes_get_size (b) : 0;
      info->is_directory = !b;
    }
  g_mutex_unlock (&self->lock);
  if (!info)
    bro_bro_error_set (error, "fs.notFound", NULL);
  return info;
}

static gboolean
fs_create_directory (BroFileSystem *fs, const char *path, gboolean parents_must_exist, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_auto (GStrv) parts = NULL;
  GString *current;
  if (!fs_not_frozen (self, "createDirectory", path, error))
    return FALSE;
  parts = g_strsplit (path, "/", -1);
  current = g_string_new ("");
  g_mutex_lock (&self->lock);
  for (guint i = 0; parts[i]; i++)
    if (*parts[i])
      {
        g_string_append_printf (current, "/%s", parts[i]);
        g_hash_table_add (self->folders, g_strdup (current->str));
      }
  g_mutex_unlock (&self->lock);
  g_string_free (current, TRUE);
  return TRUE;
}

static gboolean
fs_write_atomically (BroFileSystem *fs, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_autofree char *folder = g_path_get_dirname (path);
  gboolean ok;
  if (!fs_check (self, "write", path, error))
    return FALSE;
  g_mutex_lock (&self->lock);
  ok = g_hash_table_contains (self->folders, folder);
  if (ok)
    g_hash_table_replace (self->files, g_strdup (path), g_bytes_ref (bytes));
  g_mutex_unlock (&self->lock);
  if (!ok)
    bro_bro_error_set (error, "fs.notFound", NULL);
  return ok;
}

static gboolean
child_of (const char *path, const char *prefix)
{
  gsize n = strlen (prefix);
  return strncmp (path, prefix, n) == 0 && path[n] && !strchr (path + n, '/');
}

static int
by_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

static GPtrArray *
fs_list (BroFileSystem *fs, const char *directory, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_autofree char *prefix = strcmp (directory, "/") == 0 ? g_strdup ("/") : g_strconcat (directory, "/", NULL);
  GPtrArray *found;
  GHashTableIter it;
  gpointer key;
  g_mutex_lock (&self->lock);
  if (!g_hash_table_contains (self->folders, directory))
    {
      g_mutex_unlock (&self->lock);
      not_found (error, directory);
      return NULL;
    }
  found = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_directory_entry_free);
  for (int folders = 0; folders < 2; folders++)
    {
      g_hash_table_iter_init (&it, folders ? self->folders : self->files);
      while (g_hash_table_iter_next (&it, &key, NULL))
        if (child_of (key, prefix))
          {
            BroDirectoryEntry *e = bro_directory_entry_new ();
            e->name = g_strdup ((const char *) key + strlen (prefix));
            e->is_directory = folders;
            g_ptr_array_add (found, e);
          }
    }
  g_mutex_unlock (&self->lock);
  g_ptr_array_sort (found, by_name);
  return found;
}

static GBytes *
fs_read_range (BroFileSystem *fs, const char *path, gint64 offset, int length, BroBroError **error)
{
  g_autoptr (GBytes) all = fs_read (fs, path, error);
  gsize size;
  if (!all)
    return NULL;
  size = g_bytes_get_size (all);
  if (offset >= (gint64) size)
    return g_bytes_new (NULL, 0);
  return g_bytes_new_from_bytes (all, (gsize) offset, MIN ((gsize) length, size - (gsize) offset));
}

static gboolean
fs_rename (BroFileSystem *fs, const char *from, const char *to, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_autofree char *parent = parent_of (to);
  g_autofree char *under = g_strconcat (from, "/", NULL);
  GBytes *b;
  if (!fs_check (self, "rename", from, error))
    return FALSE;
  g_mutex_lock (&self->lock);
  if (g_hash_table_contains (self->files, to) || g_hash_table_contains (self->folders, to))
    {
      g_mutex_unlock (&self->lock);
      bro_bro_error_set (error, "fs.alreadyExists", NULL);
      return FALSE;
    }
  if (!g_hash_table_contains (self->folders, parent))
    {
      g_mutex_unlock (&self->lock);
      bro_bro_error_set (error, "fs.parentMissing", NULL);
      return FALSE;
    }
  if ((b = g_hash_table_lookup (self->files, from)))
    {
      g_hash_table_replace (self->files, g_strdup (to), g_bytes_ref (b));
      g_hash_table_remove (self->files, from);
    }
  else if (g_hash_table_contains (self->folders, from))
    {
      g_autoptr (GPtrArray) moved = g_ptr_array_new_with_free_func (g_free);
      GHashTableIter it;
      gpointer key, value;
      g_hash_table_iter_init (&it, self->files);
      while (g_hash_table_iter_next (&it, &key, &value))
        if (g_str_has_prefix (key, under))
          {
            g_ptr_array_add (moved, g_strdup (key));
            g_ptr_array_add (moved, g_strconcat (to, (const char *) key + strlen (from), NULL));
          }
      for (guint i = 0; i < moved->len; i += 2)
        {
          GBytes *f = g_bytes_ref (g_hash_table_lookup (self->files, moved->pdata[i]));
          g_hash_table_remove (self->files, moved->pdata[i]);
          g_hash_table_replace (self->files, g_strdup (moved->pdata[i + 1]), f);
        }
      g_ptr_array_set_size (moved, 0);
      g_hash_table_iter_init (&it, self->folders);
      while (g_hash_table_iter_next (&it, &key, NULL))
        if (strcmp (key, from) == 0 || g_str_has_prefix (key, under))
          {
            g_ptr_array_add (moved, g_strdup (key));
            g_ptr_array_add (moved, g_strconcat (to, (const char *) key + strlen (from), NULL));
          }
      for (guint i = 0; i < moved->len; i += 2)
        {
          g_hash_table_remove (self->folders, moved->pdata[i]);
          g_hash_table_add (self->folders, g_strdup (moved->pdata[i + 1]));
        }
    }
  else
    {
      g_mutex_unlock (&self->lock);
      not_found (error, from);
      return FALSE;
    }
  g_mutex_unlock (&self->lock);
  return TRUE;
}

static gboolean
fs_remove (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_autofree char *under = g_strconcat (path, "/", NULL);
  GHashTableIter it;
  gpointer key;
  gboolean empty = TRUE;
  if (!fs_check (self, "remove", path, error))
    return FALSE;
  g_mutex_lock (&self->lock);
  if (g_hash_table_remove (self->files, path))
    {
      g_mutex_unlock (&self->lock);
      return TRUE;
    }
  if (!g_hash_table_contains (self->folders, path))
    {
      g_mutex_unlock (&self->lock);
      not_found (error, path);
      return FALSE;
    }
  for (int folders = 0; folders < 2 && empty; folders++)
    {
      g_hash_table_iter_init (&it, folders ? self->folders : self->files);
      while (empty && g_hash_table_iter_next (&it, &key, NULL))
        empty = !g_str_has_prefix (key, under);
    }
  if (empty)
    g_hash_table_remove (self->folders, path);
  g_mutex_unlock (&self->lock);
  if (!empty)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (path));
      bro_json_value_set (params, "reason", bro_json_value_new_string ("not empty"));
      bro_bro_error_set (error, "fs.failed", params);
    }
  return empty;
}

static gboolean
fs_sync (TestMemoryFs *self, const char *op, GHashTable *of, const char *path, BroBroError **error)
{
  gboolean there;
  if (!fs_check (self, op, path, error))
    return FALSE;
  g_mutex_lock (&self->lock);
  there = g_hash_table_contains (of, path);
  g_mutex_unlock (&self->lock);
  if (!there)
    not_found (error, path);
  return there;
}

static gboolean
fs_sync_file (BroFileSystem *fs, const char *path, BroBroError **error)
{
  return fs_sync (TEST_MEMORY_FS (fs), "sync", TEST_MEMORY_FS (fs)->files, path, error);
}

static gboolean
fs_sync_directory (BroFileSystem *fs, const char *path, BroBroError **error)
{
  return fs_sync (TEST_MEMORY_FS (fs), "syncDirectory", TEST_MEMORY_FS (fs)->folders, path, error);
}

static char *
ascii_lower (const char *s)
{
  char *l = g_strdup (s);
  for (char *c = l; *c; c++)
    if (*c >= 'A' && *c <= 'Z')
      *c = (char) (*c + 32);
  return l;
}

static int
by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static int
moved_by_from (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroMovedItem *const *) a)->from, (*(BroMovedItem *const *) b)->from);
}

static void
moved_add (GPtrArray *moved, const char *from, const char *to, BroMovedFunc on_moved, gpointer data)
{
  BroMovedItem *m = bro_moved_item_new ();
  m->from = g_strdup (from);
  m->to = g_strdup (to);
  g_ptr_array_add (moved, m);
  if (on_moved)
    on_moved (m, data);
}

/* Each entry of @source in turn (by name), a folder merging into a folder of the same name (ASCII case ignored), anything
 * else moving under ConflictNamer's name; an emptied folder goes. */
static gboolean
fs_merge (BroFileSystem *fs, const char *source, const char *target, GPtrArray *moved, BroMovedFunc on_moved, gpointer data,
          BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  g_autoptr (GPtrArray) entries = fs_list (fs, source, error);
  if (!entries)
    return FALSE;
  for (guint i = 0; i < entries->len; i++)
    {
      const BroDirectoryEntry *e = g_ptr_array_index (entries, i);
      g_autofree char *src = g_strconcat (source, "/", e->name, NULL);
      g_autofree char *lower = ascii_lower (e->name);
      g_autoptr (GPtrArray) existing = fs_list (fs, target, error);
      g_autoptr (GPtrArray) names = NULL;
      const BroDirectoryEntry *same = NULL;
      g_autofree char *free_name = NULL;
      g_autofree char *dst = NULL;
      if (!existing)
        return FALSE;
      names = g_ptr_array_new ();
      for (guint k = 0; k < existing->len; k++)
        {
          const BroDirectoryEntry *x = g_ptr_array_index (existing, k);
          g_autofree char *x_lower = ascii_lower (x->name);
          if (!same && strcmp (x_lower, lower) == 0)
            same = x;
          g_ptr_array_add (names, x->name);
        }
      g_ptr_array_add (names, NULL);
      if (e->is_directory && same && same->is_directory)
        {
          g_autofree char *into = g_strconcat (target, "/", same->name, NULL);
          if (!fs_merge (fs, src, into, moved, on_moved, data, error) || !fs_remove (fs, src, error))
            return FALSE;
          continue;
        }
      free_name = bro_conflict_namer_next (e->name, (const char *const *) names->pdata, e->is_directory);
      dst = g_strconcat (target, "/", free_name, NULL);
      if (!fs_rename (fs, src, dst, error))
        return FALSE;
      if (!e->is_directory)
        moved_add (moved, src, dst, on_moved, data);
      else
        {
          g_autofree char *under = g_strconcat (dst, "/", NULL);
          g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
          GHashTableIter it;
          gpointer key;
          g_mutex_lock (&self->lock);
          g_hash_table_iter_init (&it, self->files);
          while (g_hash_table_iter_next (&it, &key, NULL))
            if (g_str_has_prefix (key, under))
              g_ptr_array_add (files, g_strdup (key));
          g_mutex_unlock (&self->lock);
          g_ptr_array_sort (files, by_text);
          for (guint k = 0; k < files->len; k++)
            {
              const char *f = g_ptr_array_index (files, k);
              g_autofree char *from = g_strconcat (src, f + strlen (dst), NULL);
              moved_add (moved, from, f, on_moved, data);
            }
        }
    }
  return TRUE;
}

/* As the platform file systems do it (fs_merge); what moved, by where it came from. */
static GPtrArray *
fs_move_merging (BroFileSystem *fs, const char *from, const char *to, BroMovePolicy policy, BroMovedFunc on_moved, gpointer data,
                 BroBroError **error)
{
  g_autoptr (BroFileInfo) source = fs_stat (fs, from, error);
  g_autoptr (GPtrArray) moved = NULL;
  if (!source)
    return NULL;
  if (!source->is_directory)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "operation", bro_json_value_new_string ("move"));
      bro_json_value_set (params, "path", bro_json_value_new_string (from));
      bro_json_value_set (params, "reason", bro_json_value_new_string ("it isn't a folder"));
      bro_bro_error_set (error, "fs.failed", params);
      return NULL;
    }
  if (!fs_exists (fs, to))
    {
      if (!fs_create_directory (fs, to, TRUE, error))
        return NULL;
    }
  else
    {
      g_autoptr (BroFileInfo) target = fs_stat (fs, to, error);
      if (!target)
        return NULL;
      if (!target->is_directory)
        {
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "path", bro_json_value_new_string (to));
          bro_bro_error_set (error, "fs.alreadyExists", params);
          return NULL;
        }
    }
  moved = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_moved_item_free);
  if (!fs_merge (fs, from, to, moved, on_moved, data, error))
    return NULL;
  g_ptr_array_sort (moved, moved_by_from);
  return g_steal_pointer (&moved);
}

static gboolean
fs_move_to_trash (BroFileSystem *fs, const char *path, const char *trash, BroBroError **error)
{
  bro_bro_error_set (error, "fs.failed", NULL); /* not faked */
  return FALSE;
}

static BroVolumeInfo *
fs_volume (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  BroVolumeInfo *v = NULL;
  g_mutex_lock (&self->lock);
  if (self->any_volume)
    {
      v = bro_volume_info_new ();
      v->id = g_strdup (self->any_volume->id);
      v->free_bytes = self->any_volume->free_bytes;
      v->fs_type = g_strdup (self->any_volume->fs_type);
      v->case_sensitive = self->any_volume->case_sensitive;
    }
  g_mutex_unlock (&self->lock);
  if (!v)
    not_found (error, path);
  return v;
}

/* A file read a few bytes at a time ('read' fails are checked on each read). */
#define TEST_TYPE_MEMORY_STREAM (test_memory_stream_get_type ())
G_DECLARE_FINAL_TYPE (TestMemoryStream, test_memory_stream, TEST, MEMORY_STREAM, GObject)

struct _TestMemoryStream {
  GObject parent_instance;
  TestMemoryFs *fs; /* a reference */
  char *path;
  GBytes *bytes;
  gsize at;
};

static GBytes *
stream_read (BroByteStream *s, int max_bytes, BroBroError **error)
{
  TestMemoryStream *self = TEST_MEMORY_STREAM (s);
  gsize size = g_bytes_get_size (self->bytes);
  gsize n = MIN ((gsize) MIN (max_bytes, 16), size - self->at);
  GBytes *piece;
  if (!fs_check (self->fs, "read", self->path, error))
    return NULL;
  piece = g_bytes_new_from_bytes (self->bytes, self->at, n);
  self->at += n;
  return piece;
}

static void stream_close (BroByteStream *s) {}

static void
stream_iface_init (BroByteStreamInterface *iface)
{
  iface->read = stream_read;
  iface->close = stream_close;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (TestMemoryStream, test_memory_stream, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_BYTE_STREAM, stream_iface_init))

static void
test_memory_stream_finalize (GObject *object)
{
  TestMemoryStream *self = TEST_MEMORY_STREAM (object);
  g_object_unref (self->fs);
  g_free (self->path);
  g_bytes_unref (self->bytes);
  G_OBJECT_CLASS (test_memory_stream_parent_class)->finalize (object);
}

static void test_memory_stream_class_init (TestMemoryStreamClass *k) { G_OBJECT_CLASS (k)->finalize = test_memory_stream_finalize; }
static void test_memory_stream_init (TestMemoryStream *self) {}

static BroByteStream *
fs_open_for_reading (BroFileSystem *fs, const char *path, gboolean bypass_cache, BroBroError **error)
{
  TestMemoryFs *self = TEST_MEMORY_FS (fs);
  TestMemoryStream *stream;
  GBytes *b;
  if (!fs_check (self, "open", path, error))
    return NULL;
  g_mutex_lock (&self->lock);
  b = g_hash_table_lookup (self->files, path);
  b = b ? g_bytes_ref (b) : NULL;
  g_mutex_unlock (&self->lock);
  if (!b)
    {
      not_found (error, path);
      return NULL;
    }
  stream = g_object_new (TEST_TYPE_MEMORY_STREAM, NULL);
  stream->fs = g_object_ref (self);
  stream->path = g_strdup (path);
  stream->bytes = b;
  return BRO_BYTE_STREAM (stream);
}

static void
memory_fs_iface_init (BroFileSystemInterface *iface)
{
  iface->list = fs_list;
  iface->read_range = fs_read_range;
  iface->rename = fs_rename;
  iface->move_merging = fs_move_merging;
  iface->remove = fs_remove;
  iface->move_to_trash = fs_move_to_trash;
  iface->sync_file = fs_sync_file;
  iface->sync_directory = fs_sync_directory;
  iface->open_for_reading = fs_open_for_reading;
  iface->volume = fs_volume;
  iface->exists = fs_exists;
  iface->read = fs_read;
  iface->stat = fs_stat;
  iface->create_directory = fs_create_directory;
  iface->write_atomically = fs_write_atomically;
}

static void
test_memory_fs_finalize (GObject *object)
{
  TestMemoryFs *self = TEST_MEMORY_FS (object);
  g_hash_table_unref (self->files);
  g_hash_table_unref (self->folders);
  g_hash_table_unref (self->read_fails);
  g_ptr_array_unref (self->fails);
  g_ptr_array_unref (self->fail_errors);
  bro_volume_info_free (self->any_volume);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_memory_fs_parent_class)->finalize (object);
}

static void
test_memory_fs_class_init (TestMemoryFsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_memory_fs_finalize;
}

static void
test_memory_fs_init (TestMemoryFs *self)
{
  g_mutex_init (&self->lock);
  self->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
  self->folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add (self->folders, g_strdup ("/"));
  self->read_fails = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_bro_error_free);
  self->fails = g_ptr_array_new_with_free_func (g_free);
  self->fail_errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_error_free);
}


/* ---- ids that count: 00000000-0000-4000-8000-000000000001, ... after the given jobs' ---------------------------- */

static void test_ids_iface_init (BroIdGeneratorInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestIds, test_ids, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_ID_GENERATOR, test_ids_iface_init))

BroId
bro_test_counted_id (int n)
{
  BroId id;
  g_snprintf (id.value, sizeof id.value, "00000000-0000-4000-8000-%012x", n);
  return id;
}

static BroId
test_ids_next (BroIdGenerator *ids)
{
  return bro_test_counted_id (g_atomic_int_add (&TEST_IDS (ids)->n, 1) + 1);
}

static void
test_ids_iface_init (BroIdGeneratorInterface *iface)
{
  iface->next = test_ids_next;
}

static void
test_ids_class_init (TestIdsClass *klass)
{
}

static void
test_ids_init (TestIds *self)
{
}

/* ---- the operating system's drives ------------------------------------------------------------------------------ */

struct _TestFakeDrives {
  GObject parent_instance;
  GMutex lock;
  GPtrArray *drives;     /* BroOsDrive * */
  GHashTable *contents;  /* device → BroDiscContent */
  GPtrArray *calls;      /* char * */
  BroDeviceSinkFunc sink; /* NULL: not monitoring */
  gpointer sink_data;
  GDestroyNotify sink_destroy;
  TestTrayFunc on_eject, on_close_tray; /* nullable */
  gpointer tray_data;
};

static void fake_drives_monitor_iface (BroDeviceMonitorInterface *iface);
static void fake_drives_control_iface (BroDriveControlInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestFakeDrives, test_fake_drives, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DEVICE_MONITOR, fake_drives_monitor_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, fake_drives_control_iface))

void
test_fake_drives_add (TestFakeDrives *self, const BroOsDrive *drive)
{
  g_mutex_lock (&self->lock);
  for (guint i = self->drives->len; i > 0; i--)
    if (strcmp (((BroOsDrive *) g_ptr_array_index (self->drives, i - 1))->device, drive->device) == 0)
      g_ptr_array_remove_index (self->drives, i - 1);
  g_ptr_array_add (self->drives, bro_os_drive_copy (drive));
  g_mutex_unlock (&self->lock);
}

void
test_fake_drives_set_content (TestFakeDrives *self, const char *device, BroDiscContent content)
{
  g_mutex_lock (&self->lock);
  g_hash_table_replace (self->contents, g_strdup (device), GINT_TO_POINTER (content));
  g_mutex_unlock (&self->lock);
}

void
test_fake_drives_send (TestFakeDrives *self, const BroDeviceEvent *event)
{
  BroDeviceSinkFunc sink;
  gpointer data;
  g_mutex_lock (&self->lock);
  sink = self->sink;
  data = self->sink_data;
  g_mutex_unlock (&self->lock);
  if (sink)
    sink (event, data);
}

gboolean
test_fake_drives_monitoring (TestFakeDrives *self)
{
  gboolean on;
  g_mutex_lock (&self->lock);
  on = self->sink != NULL;
  g_mutex_unlock (&self->lock);
  return on;
}

GPtrArray *
test_fake_drives_calls (TestFakeDrives *self)
{
  GPtrArray *calls = g_ptr_array_new_with_free_func (g_free);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->calls->len; i++)
    g_ptr_array_add (calls, g_strdup (g_ptr_array_index (self->calls, i)));
  g_mutex_unlock (&self->lock);
  return calls;
}

static void
fake_drives_forget_sink (TestFakeDrives *self)
{
  GDestroyNotify destroy;
  gpointer data;
  g_mutex_lock (&self->lock);
  destroy = self->sink_destroy;
  data = self->sink_data;
  self->sink = NULL;
  self->sink_data = NULL;
  self->sink_destroy = NULL;
  g_mutex_unlock (&self->lock);
  if (destroy)
    destroy (data);
}

static void
fake_drives_start (BroDeviceMonitor *m, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (m);
  fake_drives_forget_sink (self);
  g_mutex_lock (&self->lock);
  self->sink = sink;
  self->sink_data = data;
  self->sink_destroy = destroy;
  g_mutex_unlock (&self->lock);
}

static void
fake_drives_stop (BroDeviceMonitor *m)
{
  fake_drives_forget_sink (TEST_FAKE_DRIVES (m));
}

static GPtrArray *
fake_drives_current (BroDeviceMonitor *m)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (m);
  GPtrArray *drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->drives->len; i++)
    g_ptr_array_add (drives, bro_os_drive_copy (g_ptr_array_index (self->drives, i)));
  g_mutex_unlock (&self->lock);
  return drives;
}

static void
fake_drives_monitor_iface (BroDeviceMonitorInterface *iface)
{
  iface->start = fake_drives_start;
  iface->stop = fake_drives_stop;
  iface->current_drives = fake_drives_current;
}

static gboolean
fake_drives_call (BroDriveControl *c, const char *verb, const char *device)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (c);
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->calls, g_strconcat (verb, " ", device, NULL));
  g_mutex_unlock (&self->lock);
  return TRUE;
}

static gboolean
fake_drives_eject (BroDriveControl *c, const char *device, BroBroError **error)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (c);
  fake_drives_call (c, "eject", device);
  if (self->on_eject)
    self->on_eject (device, self->tray_data);
  return TRUE;
}

static gboolean
fake_drives_close_tray (BroDriveControl *c, const char *device, BroBroError **error)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (c);
  fake_drives_call (c, "closeTray", device);
  if (self->on_close_tray)
    self->on_close_tray (device, self->tray_data);
  return TRUE;
}

void
test_fake_drives_on_tray (TestFakeDrives *self, TestTrayFunc on_eject, TestTrayFunc on_close_tray, gpointer data)
{
  self->on_eject = on_eject;
  self->on_close_tray = on_close_tray;
  self->tray_data = data;
}

static char *
fake_drives_wait_for_mount (BroDriveControl *c, const char *device, BroDuration timeout, BroCancellationToken *cancel)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (c);
  char *path = NULL;
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->drives->len && !path; i++)
    {
      const BroOsDrive *d = g_ptr_array_index (self->drives, i);
      if (strcmp (d->device, device) == 0)
        path = g_strdup (d->mount_path);
    }
  g_mutex_unlock (&self->lock);
  return path;
}

static BroDiscContent
fake_drives_probe_content (BroDriveControl *c, const char *device)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (c);
  gpointer content;
  gboolean found;
  g_mutex_lock (&self->lock);
  found = g_hash_table_lookup_extended (self->contents, device, NULL, &content);
  g_mutex_unlock (&self->lock);
  return found ? (BroDiscContent) GPOINTER_TO_INT (content) : BRO_DISC_CONTENT_VIDEO;
}

static BroSectorReader *
fake_drives_open_raw (BroDriveControl *c, const char *device, BroBroError **error)
{
  bro_bro_error_set (error, "drive.sizeUnknown", NULL); /* raw sector reads aren't faked */
  return NULL;
}

static void
fake_drives_control_iface (BroDriveControlInterface *iface)
{
  iface->eject = fake_drives_eject;
  iface->close_tray = fake_drives_close_tray;
  iface->wait_for_mount = fake_drives_wait_for_mount;
  iface->probe_content = fake_drives_probe_content;
  iface->open_raw = fake_drives_open_raw;
}

static void
test_fake_drives_finalize (GObject *object)
{
  TestFakeDrives *self = TEST_FAKE_DRIVES (object);
  fake_drives_forget_sink (self);
  g_ptr_array_unref (self->drives);
  g_hash_table_unref (self->contents);
  g_ptr_array_unref (self->calls);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_fake_drives_parent_class)->finalize (object);
}

static void test_fake_drives_class_init (TestFakeDrivesClass *k) { G_OBJECT_CLASS (k)->finalize = test_fake_drives_finalize; }

static void
test_fake_drives_init (TestFakeDrives *self)
{
  g_mutex_init (&self->lock);
  self->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
  self->contents = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->calls = g_ptr_array_new_with_free_func (g_free);
}

/* ---- secrets in memory ------------------------------------------------------------------------------------------ */

struct _TestMemoryKeystore {
  GObject parent_instance;
  GMutex lock;
  gboolean system_store;
  GHashTable *values; /* name → value */
  GHashTable *fails;  /* name → BroBroError */
};

static void memory_keystore_iface (BroKeystoreInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestMemoryKeystore, test_memory_keystore, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_KEYSTORE, memory_keystore_iface))

TestMemoryKeystore *
test_memory_keystore_new (gboolean system_store)
{
  TestMemoryKeystore *self = g_object_new (TEST_TYPE_MEMORY_KEYSTORE, NULL);
  self->system_store = system_store;
  return self;
}

void
test_memory_keystore_fails (TestMemoryKeystore *self, const char *name, const BroBroError *error)
{
  g_mutex_lock (&self->lock);
  if (error)
    g_hash_table_replace (self->fails, g_strdup (name), bro_bro_error_copy (error));
  else
    g_hash_table_remove (self->fails, name);
  g_mutex_unlock (&self->lock);
}

/* FALSE (and @error) while calls about @name fail; the caller holds the lock. */
static gboolean
keystore_check (TestMemoryKeystore *self, const char *name, BroBroError **error)
{
  const BroBroError *fails = g_hash_table_lookup (self->fails, name);
  if (fails && error)
    *error = bro_bro_error_copy (fails);
  return fails == NULL;
}

static char *
keystore_get (BroKeystore *k, const char *name, BroBroError **error)
{
  TestMemoryKeystore *self = TEST_MEMORY_KEYSTORE (k);
  char *value = NULL;
  g_mutex_lock (&self->lock);
  if (keystore_check (self, name, error))
    value = g_strdup (g_hash_table_lookup (self->values, name));
  g_mutex_unlock (&self->lock);
  return value;
}

static gboolean
keystore_set (BroKeystore *k, const char *name, const char *value, BroBroError **error)
{
  TestMemoryKeystore *self = TEST_MEMORY_KEYSTORE (k);
  gboolean ok;
  g_mutex_lock (&self->lock);
  if ((ok = keystore_check (self, name, error)))
    g_hash_table_replace (self->values, g_strdup (name), g_strdup (value));
  g_mutex_unlock (&self->lock);
  return ok;
}

static gboolean
keystore_remove (BroKeystore *k, const char *name, BroBroError **error)
{
  TestMemoryKeystore *self = TEST_MEMORY_KEYSTORE (k);
  gboolean ok;
  g_mutex_lock (&self->lock);
  if ((ok = keystore_check (self, name, error)))
    g_hash_table_remove (self->values, name);
  g_mutex_unlock (&self->lock);
  return ok;
}

static gboolean
keystore_is_system_store (BroKeystore *k)
{
  return TEST_MEMORY_KEYSTORE (k)->system_store;
}

static void
memory_keystore_iface (BroKeystoreInterface *iface)
{
  iface->get = keystore_get;
  iface->set = keystore_set;
  iface->remove = keystore_remove;
  iface->is_system_store = keystore_is_system_store;
}

static void
test_memory_keystore_finalize (GObject *object)
{
  TestMemoryKeystore *self = TEST_MEMORY_KEYSTORE (object);
  g_hash_table_unref (self->values);
  g_hash_table_unref (self->fails);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_memory_keystore_parent_class)->finalize (object);
}

static void test_memory_keystore_class_init (TestMemoryKeystoreClass *k) { G_OBJECT_CLASS (k)->finalize = test_memory_keystore_finalize; }

static void
test_memory_keystore_init (TestMemoryKeystore *self)
{
  g_mutex_init (&self->lock);
  self->values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->fails = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_bro_error_free);
}

/* ---- an engine log that keeps its lines ------------------------------------------------------------------------- */

struct _TestRecordingEngineLog {
  GObject parent_instance;
  GMutex lock;
  GPtrArray *lines; /* BroBroMessage * */
};

static void recording_engine_log_iface (BroEngineLogInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestRecordingEngineLog, test_recording_engine_log, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_ENGINE_LOG, recording_engine_log_iface))

static void
recording_engine_log_log (BroEngineLog *l, const BroBroMessage *message)
{
  TestRecordingEngineLog *self = TEST_RECORDING_ENGINE_LOG (l);
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->lines, bro_bro_message_copy (message));
  g_mutex_unlock (&self->lock);
}

GPtrArray *
test_recording_engine_log_lines (TestRecordingEngineLog *self)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->lines->len; i++)
    g_ptr_array_add (lines, bro_bro_message_copy (g_ptr_array_index (self->lines, i)));
  g_mutex_unlock (&self->lock);
  return lines;
}

static void recording_engine_log_iface (BroEngineLogInterface *iface) { iface->log = recording_engine_log_log; }

static void
test_recording_engine_log_finalize (GObject *object)
{
  TestRecordingEngineLog *self = TEST_RECORDING_ENGINE_LOG (object);
  g_ptr_array_unref (self->lines);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_recording_engine_log_parent_class)->finalize (object);
}

static void
test_recording_engine_log_class_init (TestRecordingEngineLogClass *k)
{
  G_OBJECT_CLASS (k)->finalize = test_recording_engine_log_finalize;
}

static void
test_recording_engine_log_init (TestRecordingEngineLog *self)
{
  g_mutex_init (&self->lock);
  self->lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
}
