/* bro-test-kit.c */
#include "bro-test-kit.h"

#include <glib/gstdio.h>

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

static void test_script_iface_init (BroRunningProcessInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestScript, test_script, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_RUNNING_PROCESS, test_script_iface_init))

static BroOutputLine *
script_lines (BroRunningProcess *p)
{
  TestScript *self = TEST_SCRIPT (p);
  BroOutputLine *line;
  if (!self->stopped && self->handed > 0 && self->handed == self->cancel_after && self->cancel)
    bro_cancellation_source_cancel (self->cancel);
  if (!self->stopped && self->handed > 0 && self->handed == self->delete_after && self->delete_dir)
    bro_test_remove_tree (self->delete_dir);
  if (!self->stopped && self->lines && self->lines[self->handed])
    {
      line = bro_output_line_new ();
      line->stream = BRO_OUTPUT_SOURCE_STDOUT;
      for (guint i = 0; self->stderr_at && i < self->stderr_at->len; i++)
        if (g_array_index (self->stderr_at, int, i) == self->handed)
          line->stream = BRO_OUTPUT_SOURCE_STDERR;
      line->text = g_strdup (self->lines[self->handed++]);
      return line;
    }
  if (!self->stopped && self->stalls)
    self->exit = (BroProcessExit) { -1, 15, self->stall_seconds, FALSE, FALSE };
  else if (self->stopped)
    self->exit = (BroProcessExit) { -1, 15, -1, FALSE, self->reason == BRO_STOP_REASON_CANCELLED || self->reason == BRO_STOP_REASON_SHUTDOWN };
  else
    {
      static const char *const one_title[] = { "title_t00.mkv", NULL };
      for (const char *const *name = self->write_files ? (const char *const *) self->write_files : one_title; self->write_file_in && *name; name++)
        {
          g_autofree char *f = **name ? g_build_filename (self->write_file_in, *name, NULL) : g_strdup (self->write_file_in);
          g_autofree char *dir = g_path_get_dirname (f);
          g_mkdir_with_parents (dir, 0755);
          g_file_set_contents (f, "", 0, NULL);
        }
      for (int i = 0; self->split_parts > 0 && self->arguments && self->arguments[i]; i++)
        if (g_str_equal (self->arguments[i], "-o") && self->arguments[i + 1])
          {
            for (int k = 1; k <= self->split_parts; k++)
              {
                g_autoptr (GString) part = g_string_new (self->arguments[i + 1]);
                g_autofree char *number = g_strdup_printf ("%03d", k);
                g_string_replace (part, "%03d", number, 0);
                g_file_set_contents (part->str, "", 0, NULL);
              }
            self->split_parts = 0;
            break;
          }
      if (self->write_index >= 0 && self->arguments && (int) g_strv_length (self->arguments) > self->write_index)
        {
          g_file_set_contents (self->arguments[self->write_index], self->write_text, -1, NULL);
          self->write_index = -1;
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
  G_OBJECT_CLASS (test_script_parent_class)->finalize (o);
}

static void test_script_class_init (TestScriptClass *k) { G_OBJECT_CLASS (k)->finalize = test_script_finalize; }
static void test_script_init (TestScript *self) { self->cancel_after = -1; self->delete_after = -1; self->write_index = -1; }

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
  g_ptr_array_add (self->started, spec_copy (spec));
  script->lines = g_strdupv (self->lines);
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
  g_set_object (&self->last, script);
  return BRO_RUNNING_PROCESS (script);
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
  G_OBJECT_CLASS (test_scripted_launcher_parent_class)->finalize (o);
}

static void test_scripted_launcher_class_init (TestScriptedLauncherClass *k) { G_OBJECT_CLASS (k)->finalize = test_scripted_launcher_finalize; }

static void
test_scripted_launcher_init (TestScriptedLauncher *self)
{
  self->started = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_process_spec_free);
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

static void rl_first (BroIsolationLease *l) { g_ptr_array_add (TEST_RECORDING_LEASE (l)->owner->log, g_strdup ("firstOutput")); }
static BroBroMessage *
rl_release (BroIsolationLease *l)
{
  TestRecordingIsolation *owner = TEST_RECORDING_LEASE (l)->owner;
  BroJsonValue *params;
  g_ptr_array_add (owner->log, g_strdup ("release"));
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
  g_ptr_array_add (self->log, g_strdup ("prepare"));
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
  g_ptr_array_add (self->log, g_string_free (line, FALSE));
  return g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
}

static void
test_recording_isolation_finalize (GObject *o)
{
  g_ptr_array_unref (TEST_RECORDING_ISOLATION (o)->log);
  g_free (TEST_RECORDING_ISOLATION (o)->release_problem);
  G_OBJECT_CLASS (test_recording_isolation_parent_class)->finalize (o);
}

static void test_recording_isolation_class_init (TestRecordingIsolationClass *k) { G_OBJECT_CLASS (k)->finalize = test_recording_isolation_finalize; }
static void test_recording_isolation_init (TestRecordingIsolation *self) { self->log = g_ptr_array_new_with_free_func (g_free); }
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
    info->path = g_strdup (self->path);
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
  G_OBJECT_CLASS (test_fixed_locator_parent_class)->finalize (o);
}
static void test_fixed_locator_class_init (TestFixedLocatorClass *k) { G_OBJECT_CLASS (k)->finalize = test_fixed_locator_finalize; }
static void test_fixed_locator_init (TestFixedLocator *self) {}
static void test_fixed_locator_iface_init (BroToolLocatorInterface *iface) { iface->locate = fl_locate; }
