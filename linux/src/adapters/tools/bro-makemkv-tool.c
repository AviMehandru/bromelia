/* bro-makemkv-tool.c */
#include "bro-makemkv-tool.h"

#include "bro-listing-builder.h"
#include "bro-makemkv-args.h"
#include "bro-message-code.h"
#include "bro-robot.h"
#include "bro-run-accumulator.h"
#include "bro-tool-run.h"
#include <string.h>

struct _BroMakemkvTool {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroFileSystem *fs;
  BroSettingsIsolation *isolation;
  BroToolLocator *locator;
};

G_DEFINE_FINAL_TYPE (BroMakemkvTool, bro_makemkv_tool, G_TYPE_OBJECT)

/* makemkvcon's path, or NULL and @error set (the locator's reason). */
static char *
executable (BroMakemkvTool *self, BroBroError **error)
{
  g_autoptr (BroToolInfo) info = bro_tool_locator_locate (self->locator, BRO_TOOL_KIND_MAKEMKVCON);
  if (info->path)
    return g_strdup (info->path);
  if (error)
    {
      if (info->why)
        *error = bro_bro_message_to_error (info->why, NULL);
      else
        {
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "tool", bro_json_value_new_string ("makemkvcon"));
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_TOOL_MISSING), params);
        }
    }
  return NULL;
}

/* Called for each event; returns TRUE to stop the process (acted on once). */
typedef gboolean (*EventFunc) (const BroRobotEvent *event, gpointer data);

typedef struct {
  EventFunc func;
  gpointer data;
} Feed;

static gboolean
feed_line (const BroOutputLine *line, gpointer data, BroStopReason *reason)
{
  Feed *f = data;
  BroRobotEvent *event = bro_robot_parse_line (line->text);
  gboolean stop;
  if (!event)
    {
      event = bro_robot_event_new (BRO_ROBOT_EVENT_RAW);
      event->text = g_strdup (line->text);
    }
  stop = f->func (event, f->data);
  bro_robot_event_free (event);
  *reason = BRO_STOP_REASON_POLICY;
  return stop;
}

/* Runs @spec and feeds its lines to @func as robot events; *@transcript_problem (nullable) gets
 * process.noTranscript when the transcript couldn't be written. FALSE and @error set when it can't start. */
static gboolean
feed (BroMakemkvTool *self, const BroProcessSpec *spec, BroCancellationToken *cancel, EventFunc func, gpointer data, BroProcessExit *exit,
      BroBroMessage **transcript_problem, BroBroError **error)
{
  Feed f = { func, data };
  return bro_tool_run (self->launcher, spec, cancel, NULL, feed_line, &f, exit, transcript_problem, error);
}

static gboolean
collect_drive (const BroRobotEvent *event, gpointer data)
{
  BroMakemkvDrive *d = bro_makemkv_drive_from (event);
  if (d)
    g_ptr_array_add (data, d);
  return FALSE;
}

GPtrArray *
bro_makemkv_tool_scan_drives (BroMakemkvTool *self, BroCancellationToken *cancel, BroBroError **error)
{
  g_autofree char *exe = executable (self, error);
  g_autoptr (BroProcessSpec) spec = NULL;
  GPtrArray *drives;
  BroProcessExit exit;
  if (!exe)
    return NULL;
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (exe);
  g_strfreev (spec->arguments);
  spec->arguments = bro_makemkv_args_scan_drives ();
  spec->stop_policy = BRO_STOP_POLICY_TERMINATE_FIRST;
  drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_makemkv_drive_free);
  if (!feed (self, spec, cancel, collect_drive, drives, &exit, NULL, error))
    g_clear_pointer (&drives, g_ptr_array_unref);
  return drives;
}

/* The names in the destination before the run: none when it isn't there yet. NULL and @error set when it can't be
 * listed: every name in it would otherwise look new. */
static GHashTable *
names_before (BroMakemkvTool *self, const char *destination, BroBroError **error)
{
  g_autoptr (BroBroError) failure = NULL;
  g_autoptr (GPtrArray) entries = bro_file_system_list (self->fs, destination, &failure);
  GHashTable *set;
  if (!entries && (!failure || g_strcmp0 (failure->code, bro_message_code_wire (BRO_MSG_FS_NOT_FOUND)) != 0))
    {
      if (error)
        *error = g_steal_pointer (&failure);
      return NULL;
    }
  set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; entries && i < entries->len; i++)
    g_hash_table_add (set, g_strdup (((BroDirectoryEntry *) entries->pdata[i])->name));
  return set;
}

/* The names (char *) in the destination that weren't there before; the destination's own name when the run made it a
 * file (a backup to an .iso image). NULL and @error set when it can't be listed. */
static GPtrArray *
new_names (BroMakemkvTool *self, const char *destination, GHashTable *before, BroBroError **error)
{
  g_autoptr (BroFileInfo) info = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  GPtrArray *out;
  if (!bro_file_system_exists (self->fs, destination))
    return g_ptr_array_new_with_free_func (g_free);
  info = bro_file_system_stat (self->fs, destination, error);
  if (!info)
    return NULL;
  out = g_ptr_array_new_with_free_func (g_free);
  if (!info->is_directory)
    {
      g_ptr_array_add (out, g_path_get_basename (destination));
      return out;
    }
  entries = bro_file_system_list (self->fs, destination, error);
  if (!entries)
    {
      g_ptr_array_unref (out);
      return NULL;
    }
  for (guint i = 0; i < entries->len; i++)
    {
      const char *name = ((BroDirectoryEntry *) entries->pdata[i])->name;
      if (!g_hash_table_contains (before, name))
        g_ptr_array_add (out, g_strdup (name));
    }
  return out;
}

typedef struct {
  BroIsolationLease *lease;
  gboolean first;
  BroRunAccumulator *accumulator;
  BroListingBuilder *builder; /* nullable */
  BroRunSink *sink;
} RunState;

static gboolean
on_event (const BroRobotEvent *event, gpointer data)
{
  RunState *st = data;
  if (st->first)
    {
      st->first = FALSE;
      bro_isolation_lease_first_output (st->lease);
    }
  bro_run_accumulator_feed (st->accumulator, event);
  if (st->builder)
    bro_listing_builder_feed (st->builder, event);
  if (st->sink)
    bro_run_sink_event (st->sink, event);
  return st->accumulator->stop_reason != NULL;
}

typedef enum { CALL_INFO, CALL_MKV, CALL_BACKUP } Call;

static BroMakemkvRun *
isolated (BroMakemkvTool *self, Call call, BroRunProduct product, const BroMakemkvSource *source, const char *title, gboolean decrypt, const char *destination,
          const BroMakemkvInvocation *invocation, BroRunAccumulator *accumulator, BroListingBuilder *builder, BroRunSink *sink,
          BroCancellationToken *cancel, BroBroError **error)
{
  g_autofree char *exe = NULL;
  g_autoptr (GHashTable) before = NULL;
  g_autoptr (GPtrArray) produced = NULL;
  g_autoptr (BroBroMessage) transcript_problem = NULL;
  g_autoptr (BroIsolationLease) lease = NULL;
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autofree char *profile = NULL;
  BroMakemkvOptions options = invocation->options;
  BroMakemkvRun *run;
  BroProcessExit exit;
  RunState st = { 0 };
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return NULL;
    }
  exe = executable (self, error);
  if (!exe)
    return NULL;
  if (destination && !(before = names_before (self, destination, error)))
    return NULL;
  lease = bro_settings_isolation_prepare (self->isolation, invocation->settings, error);
  if (!lease)
    return NULL;
  profile = bro_isolation_lease_profile_path (lease);
  if (profile)
    options.profile_path = profile;
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (exe);
  g_strfreev (spec->arguments);
  switch (call)
    {
    case CALL_INFO: spec->arguments = bro_makemkv_args_info (source, &options); break;
    case CALL_MKV: spec->arguments = bro_makemkv_args_mkv (source, title, destination, &options); break;
    case CALL_BACKUP: spec->arguments = bro_makemkv_args_backup (source, decrypt, destination, &options, NULL); break;
    }
  g_hash_table_unref (spec->environment);
  spec->environment = bro_isolation_lease_environment (lease);
  spec->working_directory = g_strdup (invocation->settings->work_directory);
  spec->stop_policy = BRO_STOP_POLICY_TERMINATE_FIRST;
  spec->has_stall_timeout = invocation->has_stall_timeout;
  spec->stall_timeout = invocation->stall_timeout;
  spec->transcript = g_strdup (invocation->transcript);
  st = (RunState) { lease, TRUE, accumulator, builder, sink };
  if (!feed (self, spec, cancel, on_event, &st, &exit, &transcript_problem, error))
    {
      bro_isolation_lease_release (lease);
      return NULL;
    }
  bro_isolation_lease_release (lease);
  produced = destination ? new_names (self, destination, before, error) : g_ptr_array_new_with_free_func (g_free);
  if (!produced)
    return NULL;
  run = bro_makemkv_run_new ();
  run->outcome = bro_run_outcome_classify (accumulator, &exit, product, produced);
  run->notice = accumulator->problem ? bro_makemkv_notice_copy (accumulator->problem) : NULL;
  run->libre_drive = g_strdup (accumulator->libre_drive);
  run->version = g_strdup (accumulator->makemkv_version);
  run->transcript_problem = g_steal_pointer (&transcript_problem);
  return run;
}

BroListingRun *
bro_makemkv_tool_listing (BroMakemkvTool *self, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation, BroRunSink *sink,
                          BroCancellationToken *cancel, BroBroError **error)
{
  BroRunAccumulator *accumulator = bro_run_accumulator_new (FALSE, -1, NULL);
  BroListingBuilder *builder = bro_listing_builder_new ();
  BroMakemkvRun *run = isolated (self, CALL_INFO, BRO_RUN_PRODUCT_NOTHING, source, NULL, FALSE, NULL, invocation, accumulator, builder, sink, cancel, error);
  BroListingRun *result = NULL;
  if (run)
    {
      result = bro_listing_run_new ();
      result->listing = bro_listing_builder_build (builder);
      result->run = run;
    }
  bro_listing_builder_free (builder);
  bro_run_accumulator_free (accumulator);
  return result;
}

BroMakemkvRun *
bro_makemkv_tool_rip (BroMakemkvTool *self, const BroMakemkvSource *source, const char *title, const char *destination,
                      const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  BroRunAccumulator *accumulator = bro_run_accumulator_new (TRUE, -1, NULL);
  BroMakemkvRun *run = isolated (self, CALL_MKV, BRO_RUN_PRODUCT_TITLES, source, title, FALSE, destination, invocation, accumulator, NULL, sink, cancel, error);
  bro_run_accumulator_free (accumulator);
  return run;
}

BroMakemkvRun *
bro_makemkv_tool_backup (BroMakemkvTool *self, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
                         const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  g_auto (GStrv) check = bro_makemkv_args_backup (source, decrypt, destination, &invocation->options, error);
  BroRunAccumulator *accumulator;
  BroMakemkvRun *run;
  if (!check)
    return NULL; /* backup.needsDrive, before anything runs */
  accumulator = bro_run_accumulator_new (TRUE, source->index, source->device);
  run = isolated (self, CALL_BACKUP, BRO_RUN_PRODUCT_BACKUP, source, NULL, decrypt, destination, invocation, accumulator, NULL, sink, cancel, error);
  bro_run_accumulator_free (accumulator);
  return run;
}

static void
bro_makemkv_tool_finalize (GObject *object)
{
  BroMakemkvTool *self = BRO_MAKEMKV_TOOL (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->fs);
  g_clear_object (&self->isolation);
  g_clear_object (&self->locator);
  G_OBJECT_CLASS (bro_makemkv_tool_parent_class)->finalize (object);
}

static void
bro_makemkv_tool_class_init (BroMakemkvToolClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_makemkv_tool_finalize;
}

static void
bro_makemkv_tool_init (BroMakemkvTool *self)
{
}

BroMakemkvTool *
bro_makemkv_tool_new (BroProcessLauncher *launcher, BroFileSystem *fs, BroSettingsIsolation *isolation, BroToolLocator *locator)
{
  BroMakemkvTool *self = g_object_new (BRO_TYPE_MAKEMKV_TOOL, NULL);
  self->launcher = g_object_ref (launcher);
  self->fs = g_object_ref (fs);
  self->isolation = g_object_ref (isolation);
  self->locator = g_object_ref (locator);
  return self;
}
