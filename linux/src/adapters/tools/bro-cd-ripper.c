/* bro-cd-ripper.c */
#include "bro-cd-ripper.h"

#include "bro-cd-ripper-args.h"
#include "bro-message-code.h"
#include <string.h>

struct _BroCdRipper {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroToolLocator *locator;
  BroFileSystem *fs;
};

G_DEFINE_FINAL_TYPE (BroCdRipper, bro_cd_ripper, G_TYPE_OBJECT)

static void
stop_process (gpointer process, gpointer unused)
{
  bro_running_process_stop (process, BRO_STOP_REASON_CANCELLED);
}

/* @code with {tool} and, when @key is set, {@key: @value}. */
static void
failed (BroBroError **error, BroMessageCode code, const char *tool, const char *key, gint64 value)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "tool", bro_json_value_new_string (tool));
  if (key)
    bro_json_value_set (params, key, bro_json_value_new_integer (value));
  bro_bro_error_set (error, bro_message_code_wire (code), params);
}

static int
by_bytes (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

GStrv
bro_cd_ripper_rip (BroCdRipper *self, const char *device, const char *dest, const char *command, int stall_minutes, BroRunSink *sink,
                   BroCancellationToken *cancel, BroBroError **error)
{
  const BroToolKind kinds[] = { BRO_TOOL_KIND_CYANRIP, BRO_TOOL_KIND_ABCDE };
  char *paths[G_N_ELEMENTS (kinds)] = { NULL };
  const char *available[G_N_ELEMENTS (kinds) + 1] = { NULL };
  g_autoptr (BroJsonValue) other_discs = bro_json_value_new_object ();
  g_autoptr (BroCommandLine) line = NULL;
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (BroRunningProcess) process = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  g_autoptr (GPtrArray) saved = NULL;
  const char *exe = NULL;
  BroOutputLine *output;
  BroProcessExit exit;
  guint handler = 0, n = 0;
  GStrv result = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    {
      g_autoptr (BroToolInfo) info = bro_tool_locator_locate (self->locator, kinds[i]);
      paths[i] = g_strdup (info->path);
      if (info->path)
        available[n++] = bro_tool_kind_to_wire (kinds[i]);
    }
  bro_json_value_set (other_discs, "audioCommand", bro_json_value_new_string (command ? command : ""));
  line = bro_cd_ripper_args_build (other_discs, device, available);
  if (!line)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "platform", bro_json_value_new_string ("linux"));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_OTHER_AUDIO_NEEDS_RIPPER), params);
      goto out;
    }
  /* cyanrip and abcde come from the locator; anything else goes to the launcher as written (it searches PATH). */
  exe = line->executable;
  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    if (paths[i] && g_str_equal (line->executable, bro_tool_kind_to_wire (kinds[i])))
      exe = paths[i];
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      goto out;
    }
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (exe);
  g_strfreev (spec->arguments);
  spec->arguments = g_strdupv (line->arguments);
  spec->working_directory = g_strdup (dest);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  spec->has_stall_timeout = stall_minutes > 0;
  spec->stall_timeout.seconds = stall_minutes * 60.0;
  process = bro_process_launcher_start (self->launcher, spec, error);
  if (!process)
    goto out;
  if (cancel)
    handler = bro_cancellation_token_on_cancel (cancel, stop_process, process, NULL);
  while ((output = bro_running_process_lines (process)) != NULL)
    {
      g_autoptr (BroRobotEvent) event = bro_robot_event_new (BRO_ROBOT_EVENT_RAW);
      event->text = g_strdup (output->text);
      bro_run_sink_event (sink, event);
      bro_output_line_free (output);
    }
  exit = bro_running_process_wait (process);
  if (handler)
    bro_cancellation_token_disconnect (cancel, handler);
  if (exit.cancelled)
    bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
  else if (exit.stalled_seconds >= 0)
    failed (error, BRO_MSG_PROCESS_STALLED, line->executable, "minutes", stall_minutes);
  else if (exit.status != 0)
    failed (error, BRO_MSG_PROCESS_FAILED, line->executable, "status", exit.status);
  else
    {
      entries = bro_file_system_list (self->fs, dest, NULL);
      saved = g_ptr_array_new ();
      for (guint i = 0; entries && i < entries->len; i++)
        {
          BroDirectoryEntry *e = entries->pdata[i];
          if (e->name[0] != '.')
            g_ptr_array_add (saved, g_build_filename (dest, e->name, NULL));
        }
      g_ptr_array_sort (saved, by_bytes);
      if (saved->len == 0)
        failed (error, BRO_MSG_PROCESS_SAVED_NOTHING, line->executable, NULL, 0);
      else
        {
          g_ptr_array_add (saved, NULL);
          result = (GStrv) g_ptr_array_free (g_steal_pointer (&saved), FALSE);
        }
    }
out:
  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    g_free (paths[i]);
  return result;
}

static void
bro_cd_ripper_finalize (GObject *object)
{
  BroCdRipper *self = BRO_CD_RIPPER (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->locator);
  g_clear_object (&self->fs);
  G_OBJECT_CLASS (bro_cd_ripper_parent_class)->finalize (object);
}

static void bro_cd_ripper_class_init (BroCdRipperClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_cd_ripper_finalize; }
static void bro_cd_ripper_init (BroCdRipper *self) {}

BroCdRipper *
bro_cd_ripper_new (BroProcessLauncher *launcher, BroToolLocator *locator, BroFileSystem *fs)
{
  BroCdRipper *self = g_object_new (BRO_TYPE_CD_RIPPER, NULL);
  self->launcher = g_object_ref (launcher);
  self->locator = g_object_ref (locator);
  self->fs = g_object_ref (fs);
  return self;
}
