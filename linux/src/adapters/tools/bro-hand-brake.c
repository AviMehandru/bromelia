/* bro-hand-brake.c */
#include "bro-hand-brake.h"

#include "bro-hand-brake-args.h"
#include "bro-message-code.h"
#include "bro-tool-run.h"
#include <stdlib.h>
#include <string.h>

#define PROGRESS_MAX 10000

const char *const bro_hand_brake_built_in_presets[] = { "H.265 MKV 1080p30", "H.265 MKV 2160p60 4K", "H.264 MKV 1080p30", "H.264 MKV 480p30",
                                                        "Fast 1080p30", "HQ 1080p30 Surround", "Super HQ 1080p30 Surround", "Fast 2160p60 4K HEVC",
                                                        NULL };

struct _BroHandBrake {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroToolLocator *locator;
  BroClock *clock;
  char *home;
};

G_DEFINE_FINAL_TYPE (BroHandBrake, bro_hand_brake, G_TYPE_OBJECT)

typedef struct {
  GStrvBuilder *names;
  guint count;
  gboolean in_category;
} PresetList;

/* --preset-list: the names indented by four spaces under a category (a line at column 0 ending in /). */
static gboolean
preset_line (const BroOutputLine *line, gpointer data, BroStopReason *reason)
{
  PresetList *list = data;
  const char *text = line->text;
  if (text[0] != ' ')
    list->in_category = g_str_has_suffix (text, "/"); /* a category, or anything else at column 0 */
  else if (list->in_category && g_str_has_prefix (text, "    ") && text[4] && text[4] != ' ')
    {
      g_autofree char *name = g_strchomp (g_strdup (text + 4));
      g_strv_builder_add (list->names, name);
      list->count++;
    }
  return FALSE;
}

static gboolean
cancelled (BroBroError **error)
{
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
  return FALSE;
}

GStrv
bro_hand_brake_presets (BroHandBrake *self, BroCancellationToken *cancel, BroBroError **error)
{
  g_autoptr (BroToolInfo) info = bro_tool_locator_locate (self->locator, BRO_TOOL_KIND_HANDBRAKE);
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (GStrvBuilder) names = g_strv_builder_new ();
  g_auto (GStrv) found = NULL;
  const char *args[] = { "--preset-list", NULL };
  PresetList list = { names, 0, FALSE };
  BroProcessExit exit;
  if (!info->path)
    return g_strdupv ((char **) bro_hand_brake_built_in_presets);
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    return cancelled (error), NULL;
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (info->path);
  g_strfreev (spec->arguments);
  spec->arguments = g_strdupv ((char **) args);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = 60;
  if (!bro_tool_run (self->launcher, spec, cancel, NULL, preset_line, &list, &exit, NULL, error))
    return NULL;
  if (exit.cancelled)
    return cancelled (error), NULL;
  found = g_strv_builder_end (names);
  if (exit.status != 0 || list.count == 0)
    return g_strdupv ((char **) bro_hand_brake_built_in_presets);
  return g_steal_pointer (&found);
}

/* 'Encoding: task t of n, p %' as (this task's share, all tasks' share) of 10000. */
static gboolean
progress_of (const char *line, int *current, int *total)
{
  const char *p = strstr (line, "Encoding: task ");
  char *end;
  long task, tasks, whole;
  if (!p)
    return FALSE;
  p += strlen ("Encoding: task ");
  task = strtol (p, &end, 10);
  if (end == p || !g_str_has_prefix (end, " of "))
    return FALSE;
  p = end + 4;
  tasks = strtol (p, &end, 10);
  if (end == p || !g_str_has_prefix (end, ", "))
    return FALSE;
  p = end + 2;
  whole = strtol (p, &end, 10);
  if (end == p || end[0] != '.' || !g_ascii_isdigit (end[1]) || !g_ascii_isdigit (end[2]))
    return FALSE;
  tasks = MAX (1, tasks);
  *current = (int) MIN (PROGRESS_MAX, whole * 100 + (end[1] - '0') * 10 + (end[2] - '0'));
  *total = (int) ((CLAMP (task - 1, 0, tasks - 1) * PROGRESS_MAX + *current) / tasks);
  return TRUE;
}

/* The timer's view of an encode: the process to stop, and whether it did. */
typedef struct {
  BroRunningProcess *process;
  gint timed_out;
} Deadline;

static void
deadline_clear (gpointer data)
{
  g_object_unref (((Deadline *) data)->process);
}

static void
deadline_release (gpointer data)
{
  g_atomic_rc_box_release_full (data, deadline_clear);
}

static void
deadline_reached (gpointer data)
{
  Deadline *d = data;
  g_atomic_int_set (&d->timed_out, 1);
  bro_running_process_stop (d->process, BRO_STOP_REASON_TIMED_OUT);
}

/* The step's executable (it must exist), or the one the locator finds. */
static char *
executable (BroHandBrake *self, const BroStepDefinition *step, BroBroError **error)
{
  g_autofree char *own = g_strstrip (g_strdup (step->handbrake.executable ? step->handbrake.executable : ""));
  g_autoptr (BroToolInfo) info = NULL;
  if (*own)
    {
      BroJsonValue *params;
      if (own[0] == '~' && (own[1] == '\0' || own[1] == '/'))
        {
          g_autofree char *home = g_strdup (self->home);
          gsize n = strlen (home);
          char *joined;
          while (n > 1 && home[n - 1] == '/')
            home[--n] = '\0';
          joined = g_strconcat (home, own + 1, NULL);
          g_free (own);
          own = joined;
        }
      if (g_file_test (own, G_FILE_TEST_IS_REGULAR))
        return g_steal_pointer (&own);
      params = bro_json_value_new_object ();
      bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (BRO_TOOL_KIND_HANDBRAKE)));
      bro_json_value_set (params, "path", bro_json_value_new_string (own));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_TOOL_NOT_FOUND_AT), params);
      return NULL;
    }
  info = bro_tool_locator_locate (self->locator, BRO_TOOL_KIND_HANDBRAKE);
  if (info->path)
    return g_strdup (info->path);
  if (error)
    {
      if (info->why)
        *error = bro_bro_message_to_error (info->why, NULL);
      else
        {
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (BRO_TOOL_KIND_HANDBRAKE)));
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_TOOL_MISSING), params);
        }
    }
  return NULL;
}

/* An encode in progress: progress and kept lines go to the sink; the step's timeout stops the process. */
typedef struct {
  BroHandBrake *self;
  const BroStepDefinition *step;
  BroRunSink *sink;
  BroProgressFilter filter;
  Deadline *deadline;   /* nullable */
  BroTimerHandle *timer; /* nullable */
} Encode;

static void
encode_started (BroRunningProcess *process, gpointer data)
{
  Encode *e = data;
  BroTimerSchedule at;
  if (e->step->timeout_seconds <= 0)
    return;
  at = (BroTimerSchedule) { BRO_TIMER_SCHEDULE_AT, { bro_clock_now (e->self->clock).unix_milliseconds + (gint64) e->step->timeout_seconds * 1000 },
                            { 0 } };
  e->deadline = g_atomic_rc_box_new0 (Deadline);
  e->deadline->process = g_object_ref (process);
  e->timer = bro_clock_timer (e->self->clock, &at, deadline_reached, g_atomic_rc_box_acquire (e->deadline), deadline_release);
}

static gboolean
encode_line (const BroOutputLine *line, gpointer data, BroStopReason *reason)
{
  Encode *e = data;
  int current, total;
  if (progress_of (line->text, &current, &total))
    {
      g_autoptr (BroRobotEvent) event = bro_robot_event_new (BRO_ROBOT_EVENT_PROGRESS_VALUE);
      event->current = current;
      event->total = total;
      event->max = PROGRESS_MAX;
      bro_run_sink_event (e->sink, event);
    }
  if (bro_hand_brake_args_keep_line (&e->filter, line->text))
    {
      g_autoptr (BroRobotEvent) event = bro_robot_event_new (BRO_ROBOT_EVENT_RAW);
      event->text = g_strdup (line->text);
      bro_run_sink_event (e->sink, event);
    }
  return FALSE;
}

gboolean
bro_hand_brake_encode (BroHandBrake *self, const BroStepDefinition *step, const char *input, const char *output, BroRunSink *sink,
                       BroCancellationToken *cancel, BroHandBrakeRun *run, BroBroError **error)
{
  g_autofree char *exe = executable (self, step, error);
  g_autoptr (BroProcessSpec) spec = NULL;
  Encode e = { self, step, sink, { 0 }, NULL, NULL };
  gboolean ran;
  if (!exe)
    return FALSE;
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    return cancelled (error);
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (exe);
  g_strfreev (spec->arguments);
  spec->arguments = bro_hand_brake_args_build (step, input, output, self->home);
  spec->working_directory = g_path_get_dirname (output);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  ran = bro_tool_run (self->launcher, spec, cancel, encode_started, encode_line, &e, &run->exit, NULL, error);
  run->timed_out = e.deadline && g_atomic_int_get (&e.deadline->timed_out);
  if (e.timer)
    {
      bro_timer_handle_cancel (e.timer);
      g_object_unref (e.timer);
    }
  if (e.deadline)
    deadline_release (e.deadline);
  if (!ran)
    return FALSE;
  if (run->exit.cancelled)
    return cancelled (error);
  return TRUE;
}

static void
bro_hand_brake_finalize (GObject *object)
{
  BroHandBrake *self = BRO_HAND_BRAKE (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->locator);
  g_clear_object (&self->clock);
  g_free (self->home);
  G_OBJECT_CLASS (bro_hand_brake_parent_class)->finalize (object);
}

static void bro_hand_brake_class_init (BroHandBrakeClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_hand_brake_finalize; }
static void bro_hand_brake_init (BroHandBrake *self) {}

BroHandBrake *
bro_hand_brake_new (BroProcessLauncher *launcher, BroToolLocator *locator, BroClock *clock, const char *home)
{
  BroHandBrake *self = g_object_new (BRO_TYPE_HAND_BRAKE, NULL);
  self->launcher = g_object_ref (launcher);
  self->locator = g_object_ref (locator);
  self->clock = g_object_ref (clock);
  self->home = g_strdup (home);
  return self;
}
