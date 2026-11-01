/* bro-scenario-tools.c */
#include "bro-scenario-tools.h"
#include "bro-scenario-catalog.h"
#include "bro-test-fixtures.h"

#include "bro-argument-splitter.h"
#include "bro-message-code.h"
#include "bro-output-line.h"
#include "bro-timer-handle.h"
#include "bro-timer-schedule.h"

#include <string.h>

#define GRACE_MS 5000
#define ABANDON_MS 30000

#define J(v, k) bro_json_value_member ((v), (k))
#define S(v, k, d) bro_json_value_get_string (J ((v), (k)), (d))

struct _BroScenarioTools {
  GObject parent_instance;
  BroScenarioWorld *world;
  TestManualClock *clock;
  TestMemoryFs *fs;
  GHashTable *named; /* the named robot lines */
  GMutex lock;
  GPtrArray *calls;     /* char * */
  GPtrArray *alive;     /* Process * (references) */
  GHashTable *backups;  /* the destination's folder → the disc backed up (BroJsonValue): reading anything there reads that
                         * disc, whatever the engine renamed the backup to */
  char *step;
  int in_step;
  BroScenarioCrashFunc crash;
  gpointer crash_data;
};

/* What a rule makes a launch do: its lines (stdout, stderr), its exit (has_exit FALSE: never), its silence before that,
 * the signals it ignores. */
typedef struct {
  GPtrArray *out; /* char * */
  GPtrArray *err; /* char * */
  gboolean has_exit;
  int exit;
  double silence; /* <= 0: none */
  GPtrArray *ignore; /* char *; NULL: none */
} Plan;

static void
plan_clear (Plan *p)
{
  g_clear_pointer (&p->out, g_ptr_array_unref);
  g_clear_pointer (&p->err, g_ptr_array_unref);
  g_clear_pointer (&p->ignore, g_ptr_array_unref);
}

static void
plan_init (Plan *p)
{
  memset (p, 0, sizeof *p);
  p->out = g_ptr_array_new_with_free_func (g_free);
  p->err = g_ptr_array_new_with_free_func (g_free);
  p->has_exit = TRUE;
}

/* ---- a process ---- */

#define TYPE_PROCESS (scenario_process_get_type ())
G_DECLARE_FINAL_TYPE (ScenarioProcess, scenario_process, SCENARIO, PROCESS, GObject)

struct _ScenarioProcess {
  GObject parent_instance;
  BroScenarioTools *tools; /* a reference */
  char *transcript;        /* nullable */
  char *executable;
  GStrv arguments;
  BroStopPolicy stop_policy;
  gboolean has_stall;
  double stall_seconds;
  Plan run;
  GMutex lock;
  GCond cond;
  GQueue *lines;    /* BroOutputLine * */
  gboolean closed;  /* no more lines */
  gint written, read;
  GPtrArray *timers; /* BroTimerHandle * */
  gboolean has_reason;
  BroStopReason reason;
  double stalled; /* < 0: not stalled */
  gboolean ended;
  gboolean finished;
  BroProcessExit exit;
  gboolean outlived; /* an ignored KILL: abandoned later */
};

static void process_iface_init (BroRunningProcessInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (ScenarioProcess, scenario_process, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_RUNNING_PROCESS, process_iface_init))

/* Its lines are all read (it may still be running). */
static gboolean
drained (ScenarioProcess *p)
{
  return g_atomic_int_get (&p->read) == g_atomic_int_get (&p->written);
}

/* Appends a line to a transcript (not once the files are frozen: a killed engine's). */
static void
append (BroScenarioTools *tools, const char *transcript, const char *line)
{
  BroFileSystem *fs = BRO_FILE_SYSTEM (tools->fs);
  g_autoptr (GBytes) before = NULL;
  g_autoptr (BroBroError) ignored = NULL;
  GByteArray *all;
  if (!transcript || !*transcript || test_memory_fs_frozen (tools->fs))
    return;
  if (bro_file_system_exists (fs, transcript))
    before = bro_file_system_read (fs, transcript, &ignored);
  all = g_byte_array_new ();
  if (before)
    g_byte_array_append (all, g_bytes_get_data (before, NULL), g_bytes_get_size (before));
  g_byte_array_append (all, (const guint8 *) line, strlen (line));
  g_byte_array_append (all, (const guint8 *) "\n", 1);
  test_memory_fs_put (tools->fs, transcript, all->data, all->len);
  g_byte_array_unref (all);
}

static char *
stamp (BroScenarioTools *tools, const char *what)
{
  g_autofree char *at = bro_instant_format (bro_clock_now (BRO_CLOCK (tools->clock)));
  return g_strdup_printf ("==== %s %s", at, what);
}

static void
emit (ScenarioProcess *p, BroOutputSource source, const char *text)
{
  BroOutputLine *line = bro_output_line_new ();
  line->stream = source;
  line->text = g_strdup (text);
  line->at = bro_clock_now (BRO_CLOCK (p->tools->clock));
  g_atomic_int_inc (&p->written);
  g_mutex_lock (&p->lock);
  g_queue_push_tail (p->lines, line);
  g_cond_broadcast (&p->cond);
  g_mutex_unlock (&p->lock);
  append (p->tools, p->transcript, text);
}

static void
tools_ended (BroScenarioTools *tools, ScenarioProcess *p)
{
  g_mutex_lock (&tools->lock);
  g_ptr_array_remove (tools->alive, p);
  g_mutex_unlock (&tools->lock);
}

/* exit status 0, signal 15 or abandoned, then , stalled and , cancelled, as the platform launchers end a transcript. */
static char *
ending (BroProcessExit exit)
{
  GString *s = g_string_new (NULL);
  if (exit.abandoned)
    g_string_append (s, "abandoned");
  else if (exit.signal)
    g_string_append_printf (s, "signal %d", exit.signal);
  else
    g_string_append_printf (s, "exit status %d", exit.status);
  if (exit.stalled_seconds >= 0)
    g_string_append (s, ", stalled");
  if (exit.cancelled)
    g_string_append (s, ", cancelled");
  return g_string_free (s, FALSE);
}

/* Takes the timers out (to cancel outside the lock). */
static GPtrArray *
take_timers (ScenarioProcess *p)
{
  GPtrArray *t = p->timers;
  p->timers = g_ptr_array_new_with_free_func (g_object_unref);
  return t;
}

static void
cancel_timers (GPtrArray *timers)
{
  for (guint i = 0; i < timers->len; i++)
    bro_timer_handle_cancel (timers->pdata[i]);
  g_ptr_array_unref (timers);
}

static void
end (ScenarioProcess *p, BroProcessExit exit, gboolean killed)
{
  GPtrArray *timers;
  g_mutex_lock (&p->lock);
  if (p->ended)
    {
      g_mutex_unlock (&p->lock);
      return;
    }
  p->ended = TRUE;
  timers = take_timers (p);
  g_mutex_unlock (&p->lock);
  cancel_timers (timers);
  if (!killed)
    {
      g_autofree char *how = ending (exit);
      g_autofree char *line = stamp (p->tools, how);
      append (p->tools, p->transcript, line);
      tools_ended (p->tools, p);
    }
  g_mutex_lock (&p->lock);
  p->closed = TRUE;
  p->finished = TRUE;
  p->exit = exit;
  g_cond_broadcast (&p->cond);
  g_mutex_unlock (&p->lock);
}

static gboolean
cancelled_reason (ScenarioProcess *p)
{
  gboolean c;
  g_mutex_lock (&p->lock);
  c = p->has_reason && (p->reason == BRO_STOP_REASON_CANCELLED || p->reason == BRO_STOP_REASON_SHUTDOWN);
  g_mutex_unlock (&p->lock);
  return c;
}

typedef enum { AT_NATURAL, AT_STALL, AT_SIGNAL, AT_ABANDON } TimerAction;

typedef struct {
  ScenarioProcess *p; /* a reference */
  TimerAction action;
  int signal_index;
} Timed;

static void
timed_free (gpointer data)
{
  Timed *t = data;
  g_object_unref (t->p);
  g_free (t);
}

static void on_timer (gpointer data);

static void
after (ScenarioProcess *p, gint64 ms, TimerAction action, int signal_index)
{
  g_autoptr (BroTimerSchedule) when = bro_timer_schedule_new ();
  Timed *t = g_new0 (Timed, 1);
  BroTimerHandle *handle;
  t->p = g_object_ref (p);
  t->action = action;
  t->signal_index = signal_index;
  when->kind = BRO_TIMER_SCHEDULE_AT;
  when->instant.unix_milliseconds = bro_clock_now (BRO_CLOCK (p->tools->clock)).unix_milliseconds + ms;
  handle = bro_clock_timer (BRO_CLOCK (p->tools->clock), when, on_timer, t, timed_free);
  g_mutex_lock (&p->lock);
  if (p->ended)
    {
      g_mutex_unlock (&p->lock);
      bro_timer_handle_cancel (handle);
      g_object_unref (handle);
      return;
    }
  g_ptr_array_add (p->timers, handle);
  g_mutex_unlock (&p->lock);
}

/* The end its script gives it, unless it was stopped meanwhile (exit: never stays running). */
static void
natural (ScenarioProcess *p)
{
  gboolean stopped;
  g_mutex_lock (&p->lock);
  stopped = p->has_reason;
  g_mutex_unlock (&p->lock);
  if (!stopped && p->run.has_exit)
    end (p, bro_process_exit_status (p->run.exit), FALSE);
}

/* The stall watchdog: it is quiet from now on, so it is stopped as stalled once the timeout has passed. */
static void
watch (ScenarioProcess *p)
{
  if (p->has_stall)
    after (p, (gint64) (p->stall_seconds * 1000 + 0.5), AT_STALL, 0);
}

static const char *const interrupt_first[] = { "SIGINT", "SIGTERM", "SIGKILL", NULL };
static const char *const terminate_first[] = { "SIGTERM", "SIGKILL", NULL };

static int
signal_number (const char *signal)
{
  return !strcmp (signal, "SIGINT") ? 2 : !strcmp (signal, "SIGKILL") ? 9 : 15;
}

static gboolean
ignores (ScenarioProcess *p, const char *signal)
{
  for (guint i = 0; p->run.ignore && i < p->run.ignore->len; i++)
    if (!strcmp (p->run.ignore->pdata[i], signal))
      return TRUE;
  return FALSE;
}

/* Sends signals[i]: one it doesn't ignore ends it; otherwise the next comes 5 s later, and 30 s after an ignored SIGKILL
 * it is abandoned. */
static void
send_signal (ScenarioProcess *p, int i)
{
  const char *const *signals = p->stop_policy == BRO_STOP_POLICY_INTERRUPT_FIRST ? interrupt_first : terminate_first;
  gboolean ended;
  g_mutex_lock (&p->lock);
  ended = p->ended;
  g_mutex_unlock (&p->lock);
  if (ended)
    return;
  if (!ignores (p, signals[i]))
    {
      BroProcessExit exit = { -1, signal_number (signals[i]), -1, FALSE, cancelled_reason (p) };
      g_mutex_lock (&p->lock);
      exit.stalled_seconds = p->stalled;
      g_mutex_unlock (&p->lock);
      end (p, exit, FALSE);
      return;
    }
  if (signals[i + 1])
    after (p, GRACE_MS, AT_SIGNAL, i + 1);
  else
    after (p, ABANDON_MS, AT_ABANDON, 0);
}

static void
process_stop (BroRunningProcess *rp, BroStopReason reason)
{
  ScenarioProcess *p = SCENARIO_PROCESS (rp);
  g_mutex_lock (&p->lock);
  if (p->ended || p->has_reason)
    {
      g_mutex_unlock (&p->lock);
      return;
    }
  p->has_reason = TRUE;
  p->reason = reason;
  g_mutex_unlock (&p->lock);
  send_signal (p, 0);
}

static void
on_timer (gpointer data)
{
  Timed *t = data;
  ScenarioProcess *p = t->p;
  switch (t->action)
    {
    case AT_NATURAL:
      natural (p);
      break;
    case AT_STALL:
      {
        gboolean skip;
        g_mutex_lock (&p->lock);
        skip = p->ended || p->has_reason;
        if (!skip)
          p->stalled = p->stall_seconds;
        g_mutex_unlock (&p->lock);
        if (!skip)
          process_stop (BRO_RUNNING_PROCESS (p), BRO_STOP_REASON_STALLED);
        break;
      }
    case AT_SIGNAL:
      send_signal (p, t->signal_index);
      break;
    case AT_ABANDON:
      {
        BroProcessExit exit = { -1, 0, -1, TRUE, cancelled_reason (p) };
        g_mutex_lock (&p->lock);
        exit.stalled_seconds = p->stalled;
        g_mutex_unlock (&p->lock);
        end (p, exit, FALSE);
        break;
      }
    }
}

static BroOutputLine *
process_lines (BroRunningProcess *rp)
{
  ScenarioProcess *p = SCENARIO_PROCESS (rp);
  BroOutputLine *line;
  g_mutex_lock (&p->lock);
  while (g_queue_is_empty (p->lines) && !p->closed)
    g_cond_wait (&p->cond, &p->lock);
  line = g_queue_pop_head (p->lines);
  g_mutex_unlock (&p->lock);
  if (line)
    g_atomic_int_inc (&p->read);
  return line;
}

static BroProcessExit
process_wait (BroRunningProcess *rp)
{
  ScenarioProcess *p = SCENARIO_PROCESS (rp);
  BroProcessExit exit;
  g_mutex_lock (&p->lock);
  while (!p->finished)
    g_cond_wait (&p->cond, &p->lock);
  exit = p->exit;
  g_mutex_unlock (&p->lock);
  return exit;
}

static BroBroMessage *
process_transcript_problem (BroRunningProcess *rp)
{
  return NULL;
}

static void
process_iface_init (BroRunningProcessInterface *iface)
{
  iface->lines = process_lines;
  iface->wait = process_wait;
  iface->stop = process_stop;
  iface->transcript_problem = process_transcript_problem;
}

static void
scenario_process_finalize (GObject *object)
{
  ScenarioProcess *p = SCENARIO_PROCESS (object);
  g_object_unref (p->tools);
  g_free (p->transcript);
  g_free (p->executable);
  g_strfreev (p->arguments);
  plan_clear (&p->run);
  g_queue_free_full (p->lines, (GDestroyNotify) bro_output_line_free);
  g_ptr_array_unref (p->timers);
  g_mutex_clear (&p->lock);
  g_cond_clear (&p->cond);
  G_OBJECT_CLASS (scenario_process_parent_class)->finalize (object);
}

static void
scenario_process_class_init (ScenarioProcessClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = scenario_process_finalize;
}

static void
scenario_process_init (ScenarioProcess *p)
{
  g_mutex_init (&p->lock);
  g_cond_init (&p->cond);
  p->lines = g_queue_new ();
  p->timers = g_ptr_array_new_with_free_func (g_object_unref);
  p->stalled = -1;
}

/* The process starts: the transcript's header, its lines at once, then its silence, its watchdog or its end. */
static void
begin (ScenarioProcess *p)
{
  GString *shown = g_string_new ("$");
  g_autofree char *header = NULL;
  g_autofree char *quoted = bro_argument_splitter_quote (p->executable);
  g_string_append_printf (shown, " %s", quoted);
  for (guint i = 0; p->arguments && p->arguments[i]; i++)
    {
      g_autofree char *q = bro_argument_splitter_quote (p->arguments[i]);
      g_string_append_printf (shown, " %s", q);
    }
  header = stamp (p->tools, shown->str);
  g_string_free (shown, TRUE);
  append (p->tools, p->transcript, header);
  for (guint i = 0; i < p->run.out->len; i++)
    emit (p, BRO_OUTPUT_SOURCE_STDOUT, p->run.out->pdata[i]);
  for (guint i = 0; i < p->run.err->len; i++)
    emit (p, BRO_OUTPUT_SOURCE_STDERR, p->run.err->pdata[i]);
  if (p->run.silence > 0)
    {
      after (p, (gint64) (p->run.silence * 1000 + 0.5), AT_NATURAL, 0);
      watch (p);
    }
  else if (!p->run.has_exit)
    watch (p);
  else
    natural (p);
}

/* ---- the tools ---- */

static void tools_iface_init (BroProcessLauncherInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroScenarioTools, bro_scenario_tools, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_PROCESS_LAUNCHER, tools_iface_init))

/* The arguments of a makemkvcon run: its command (info, mkv, backup, …), source, title and destination. */
typedef struct {
  char *command, *source, *title, *dest;
  int min_length; /* < 0: none */
} Args;

static void
args_clear (Args *a)
{
  g_free (a->command);
  g_free (a->source);
  g_free (a->title);
  g_free (a->dest);
}

static void
makemkv_args (GStrv a, Args *out)
{
  int at = -1;
  guint n = a ? g_strv_length (a) : 0;
  g_autoptr (GPtrArray) rest = g_ptr_array_new ();
  memset (out, 0, sizeof *out);
  out->min_length = -1;
  for (guint i = 0; i < n && at < 0; i++)
    if (!strcmp (a[i], "info") || !strcmp (a[i], "mkv") || !strcmp (a[i], "backup") || !strcmp (a[i], "reg") || !strcmp (a[i], "f"))
      at = (int) i;
  for (guint i = 0; i < n; i++)
    if (g_str_has_prefix (a[i], "--minlength=") && out->min_length < 0)
      out->min_length = (int) g_ascii_strtoll (a[i] + 12, NULL, 10);
  if (at < 0)
    {
      out->command = g_strdup (n ? a[0] : "");
      out->source = g_strdup ("");
      out->title = g_strdup ("");
      out->dest = g_strdup ("");
      return;
    }
  for (guint i = (guint) at + 1; i < n; i++)
    if (strcmp (a[i], "--decrypt") != 0)
      g_ptr_array_add (rest, a[i]);
#define REST(i) ((i) < rest->len ? (const char *) rest->pdata[(i)] : "")
  out->command = g_strdup (a[at]);
  out->source = g_strdup (REST (0));
  if (!strcmp (out->command, "mkv"))
    {
      out->title = g_strdup (REST (1));
      out->dest = g_strdup (REST (2));
    }
  else if (!strcmp (out->command, "backup"))
    {
      out->title = g_strdup ("");
      out->dest = g_strdup (REST (1));
    }
  else
    {
      out->title = g_strdup ("");
      out->dest = g_strdup ("");
    }
#undef REST
}

static const char *
option (GStrv a, const char *name)
{
  for (guint i = 0; a && a[i]; i++)
    if (!strcmp (a[i], name))
      return a[i + 1];
  return NULL;
}

static const char *
name_of (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash ? slash + 1 : path;
}

static gboolean
has_arg (GStrv a, const char *x)
{
  for (guint i = 0; a && a[i]; i++)
    if (!strcmp (a[i], x))
      return TRUE;
  return FALSE;
}

static gboolean
matches_when (BroJsonValue *when, const char *command, const Args *mk, GStrv a, const char *input)
{
  BroJsonValue *include;
  const char *c, *t, *file;
  if (!when)
    return TRUE;
  if ((c = S (when, "command", NULL)) && strcmp (c, command) != 0)
    return FALSE;
  if ((t = S (when, "title", NULL)) && (!mk || g_strcmp0 (mk->title, t) != 0))
    return FALSE;
  include = J (when, "argsInclude");
  for (guint i = 0; i < bro_json_value_length (include); i++)
    if (!has_arg (a, bro_json_value_get_string (bro_json_value_at (include, i), "")))
      return FALSE;
  if ((file = S (when, "file", NULL)) && (!input || !g_pattern_match_simple (file, input)))
    return FALSE;
  return TRUE;
}

/* mkvmerge -J's answer, in the shape MkvProbe reads. */
static char *
probe_json (double seconds, BroJsonValue *tracks, int chapters)
{
  g_autoptr (BroJsonValue) o = bro_json_value_new_object ();
  BroJsonValue *container = bro_json_value_new_object (), *properties = bro_json_value_new_object (), *list = bro_json_value_new_array ();
  BroJsonValue *chapter_list = bro_json_value_new_array ();
  char *text;
  gsize n;
  bro_json_value_set (properties, "duration", bro_json_value_new_integer ((gint64) (seconds * 1e9 + (seconds >= 0 ? 0.5 : -0.5))));
  bro_json_value_set (container, "properties", properties);
  bro_json_value_set (o, "container", container);
  for (guint i = 0; i < bro_json_value_length (tracks); i++)
    {
      BroJsonValue *t = bro_json_value_new_object ();
      bro_json_value_set (t, "id", bro_json_value_new_integer (i));
      bro_json_value_set (t, "type", bro_json_value_new_string (bro_json_value_get_string (bro_json_value_at (tracks, i), "")));
      bro_json_value_append (list, t);
    }
  bro_json_value_set (o, "tracks", list);
  if (chapters > 0)
    {
      BroJsonValue *c = bro_json_value_new_object ();
      bro_json_value_set (c, "num_entries", bro_json_value_new_integer (chapters));
      bro_json_value_append (chapter_list, c);
    }
  bro_json_value_set (o, "chapters", chapter_list);
  text = bro_json_value_to_text (o);
  n = strlen (text);
  while (n > 0 && text[n - 1] == '\n')
    text[--n] = '\0';
  return text;
}

/* What the listing says of the title a file came from (title_t<i>.mkv, else the first): its duration, a video and an
 * audio track, its chapters. */
static char *
listed_probe (BroJsonValue *disc, const char *file)
{
  BroJsonValue *titles = J (disc, "titles"), *title;
  const char *at = strstr (file, "title_t");
  guint index = 0;
  g_autoptr (BroJsonValue) tracks = bro_json_value_new_array ();
  if (at && g_ascii_isdigit (at[7]))
    index = (guint) g_ascii_strtoull (at + 7, NULL, 10);
  title = bro_json_value_at (titles, index);
  bro_json_value_append (tracks, bro_json_value_new_string ("video"));
  bro_json_value_append (tracks, bro_json_value_new_string ("audio"));
  return probe_json (bro_scenario_seconds (S (title, "duration", "0")), tracks, (int) bro_json_value_get_integer (J (title, "chapters"), 2));
}

static void
write_file (BroScenarioTools *self, const char *path, GBytes *bytes)
{
  if (test_memory_fs_frozen (self->fs))
    return;
  test_memory_fs_put (self->fs, path, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
}

static void
write_text (BroScenarioTools *self, const char *path, const char *text)
{
  g_autoptr (GBytes) b = g_bytes_new (text, strlen (text));
  write_file (self, path, b);
}

/* The disc a makemkvcon source reads: a backup the tools made holds the disc it was made from; otherwise the world's. */
static BroJsonValue *
disc_of (BroScenarioTools *self, const char *source)
{
  const char *path = g_str_has_prefix (source, "file:") || g_str_has_prefix (source, "iso:") ? strchr (source, ':') + 1 : NULL;
  if (path)
    {
      GHashTableIter it;
      gpointer k, v;
      g_mutex_lock (&self->lock);
      g_hash_table_iter_init (&it, self->backups);
      while (g_hash_table_iter_next (&it, &k, &v))
        {
          g_autofree char *prefix = g_strconcat (k, "/", NULL);
          if (g_str_has_prefix (path, prefix))
            {
              BroJsonValue *d = bro_json_value_ref (v);
              g_mutex_unlock (&self->lock);
              return d;
            }
        }
      g_mutex_unlock (&self->lock);
    }
  return bro_scenario_world_disc_of (self->world, source);
}

typedef struct {
  BroScenarioTools *self;
  const BroProcessSpec *spec;
  const Args *mk;
  const char *dest;
  const char *debug_log;
  const char *input;
  const char *output;
} Filler;

static char *
fill (const Filler *f, const char *text, const char *title)
{
  GString *s = g_string_new (text);
  g_autofree char *device = f->mk ? bro_scenario_world_device_of (f->self->world, f->mk->source) : g_strdup ("");
  GHashTableIter it;
  gpointer k, v;
  g_string_replace (s, "{title}", title && *title ? title : f->mk ? f->mk->title : "", 0);
  g_string_replace (s, "{dest}", f->dest, 0);
  g_string_replace (s, "{debugLog}", f->debug_log, 0);
  g_string_replace (s, "{device}", device, 0);
  g_string_replace (s, "{input}", f->input ? f->input : "", 0);
  g_string_replace (s, "{output}", f->output ? f->output : "", 0);
  for (guint i = 0; f->spec->arguments && f->spec->arguments[i]; i++)
    {
      g_autofree char *key = g_strdup_printf ("{arg%u}", i);
      g_string_replace (s, key, f->spec->arguments[i], 0);
    }
  if (f->spec->environment)
    {
      g_hash_table_iter_init (&it, f->spec->environment);
      while (g_hash_table_iter_next (&it, &k, &v))
        {
          g_autofree char *key = g_strconcat ("{env:", (char *) k, "}", NULL);
          g_string_replace (s, key, v, 0);
        }
    }
  return g_string_free (s, FALSE);
}

static void
add_lines (GPtrArray *to, const char *text)
{
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      gsize n = strlen (lines[i]);
      if (n > 0 && lines[i][n - 1] == '\r')
        lines[i][--n] = '\0';
      if (n > 0)
        g_ptr_array_add (to, g_strdup (lines[i]));
    }
}

static void
plan_run (BroScenarioTools *self, const char *tool, const BroProcessSpec *spec, BroJsonValue *script, Plan *out)
{
  GStrv a = spec->arguments;
  Args mk_args = { 0 };
  const Args *mk = NULL;
  const char *command, *input, *output;
  g_autofree char *dest = NULL;
  g_autofree char *debug_log = NULL;
  BroJsonValue *rules = J (script, "rules"), *rule = NULL, *v;
  g_autoptr (BroJsonValue) disc = NULL;
  Filler f;
  int written = -1;
  plan_init (out);
  if (!strcmp (tool, "makemkvcon"))
    {
      makemkv_args (a, &mk_args);
      mk = &mk_args;
    }
  command = mk ? mk->command : a && a[0] ? a[0] : "";
  input = option (a, "-i");
  if (!input && !strcmp (command, "-J") && a && a[0])
    input = a[1];
  output = option (a, "-o");
  dest = g_strdup (mk ? mk->dest : output ? output : spec->working_directory ? spec->working_directory : TEST_ROOT_DATA_DIR);
  debug_log = g_strconcat (spec->working_directory ? spec->working_directory : TEST_ROOT_DATA_DIR, "/MakeMKV_log.txt", NULL);
  f = (Filler) { self, spec, mk, dest, debug_log, input, output };

  if (mk && !strcmp (mk->command, "backup") && bro_file_system_exists (BRO_FILE_SYSTEM (self->fs), mk->dest))
    {
      const char *names[] = { "backupFolderExists", "backupFailed", "backupFailedSummary" };
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        g_ptr_array_add (out->out, fill (&f, g_hash_table_lookup (self->named, names[i]), NULL));
      out->exit = 0;
      args_clear (&mk_args);
      return;
    }
  for (guint i = 0; i < bro_json_value_length (rules) && !rule; i++)
    if (matches_when (J (bro_json_value_at (rules, i), "when"), command, mk, a, input))
      rule = bro_json_value_at (rules, i);
  if (!rule)
    {
      GString *joined = g_string_new (NULL);
      for (guint i = 0; a && a[i]; i++)
        g_string_append_printf (joined, "%s%s", i ? " " : "", a[i]);
      g_ptr_array_add (out->err, g_strdup_printf ("%s: no rule of its script matches %s", tool, joined->str));
      g_string_free (joined, TRUE);
      out->exit = 127;
      args_clear (&mk_args);
      return;
    }
  disc = mk ? disc_of (self, mk->source) : bro_scenario_world_disc (self->world);
  if (mk && !strcmp (mk->command, "backup") && disc)
    {
      const char *slash = strrchr (mk->dest, '/');
      g_mutex_lock (&self->lock);
      g_hash_table_replace (self->backups, g_strndup (mk->dest, slash ? (gsize) (slash - mk->dest) : 0), bro_json_value_ref (disc));
      g_mutex_unlock (&self->lock);
    }
  if (S (rule, "writeDebugLog", NULL))
    write_text (self, debug_log, S (rule, "writeDebugLog", ""));
  if ((v = J (rule, "writeTitles")) && mk && disc)
    {
      g_autofree char *listing = bro_scenario_listing (disc, mk->min_length);
      g_auto (GStrv) lines = g_strsplit (listing, "\n", -1);
      BroJsonValue *only = J (v, "titles");
      g_autoptr (GArray) titles = g_array_new (FALSE, FALSE, sizeof (int));
      int count = 0;
      for (guint i = 0; lines[i]; i++)
        count += g_str_has_prefix (lines[i], "TINFO:") && strstr (lines[i], ",27,0,") != NULL;
      if (!strcmp (mk->title, "all"))
        for (int i = 0; i < count; i++)
          {
            gboolean wanted = only == NULL;
            for (guint k = 0; only && k < bro_json_value_length (only); k++)
              wanted |= bro_json_value_get_integer (bro_json_value_at (only, k), -1) == i;
            if (wanted)
              g_array_append_val (titles, i);
          }
      else
        {
          char *end = NULL;
          int t = (int) g_ascii_strtoll (mk->title, &end, 10);
          if (*mk->title && end && !*end)
            g_array_append_val (titles, t);
        }
      written = (int) titles->len;
      for (guint i = 0; i < titles->len; i++)
        {
          int t = g_array_index (titles, int, i);
          g_autofree char *number = g_strdup_printf ("%d", t);
          g_autofree char *path = g_strdup_printf ("%s/title_t%02d.mkv", dest, t);
          g_autofree char *content = fill (&f, S (v, "content", ""), number);
          write_text (self, path, content);
        }
    }
  v = J (rule, "write");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      BroJsonValue *w = bro_json_value_at (v, i), *bytes;
      g_autofree char *filled = fill (&f, S (w, "path", ""), NULL);
      g_autofree char *path = filled[0] == '/' ? g_strdup (filled) : g_strconcat (dest, "/", filled, NULL);
      g_autoptr (GBytes) b = NULL;
      if (g_strcmp0 (S (w, "copyOf", NULL), "input") == 0 && input && bro_file_system_exists (BRO_FILE_SYSTEM (self->fs), input))
        {
          g_autoptr (BroBroError) ignored = NULL;
          b = bro_file_system_read (BRO_FILE_SYSTEM (self->fs), input, &ignored);
        }
      else if ((bytes = J (w, "bytes")))
        b = bro_scenario_file_bytes (bytes);
      else
        {
          g_autofree char *content = fill (&f, S (w, "content", ""), NULL);
          b = g_bytes_new (content, strlen (content));
        }
      if (b)
        write_file (self, path, b);
    }
  if ((v = J (rule, "writeIso")))
    {
      g_autoptr (GBytes) b = bro_scenario_file_bytes (v);
      write_file (self, dest, b);
    }
  v = J (rule, "writeFolder");
  for (guint i = 0; v && v->kind == BRO_JSON_VALUE_OBJECT && i < v->keys->len; i++)
    {
      g_autofree char *path = g_strconcat (dest, "/", (char *) v->keys->pdata[i], NULL);
      g_autofree char *content = fill (&f, bro_json_value_get_string (v->items->pdata[i], ""), NULL);
      write_text (self, path, content);
    }

  v = J (rule, "print");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *text = bro_json_value_get_string (bro_json_value_at (v, i), "");
      const char *named = g_hash_table_lookup (self->named, text);
      if (!strcmp (text, "listing"))
        {
          if (disc)
            {
              g_autofree char *listing = bro_scenario_listing (disc, mk ? mk->min_length : -1);
              add_lines (out->out, listing);
            }
        }
      else if (g_str_has_prefix (text, "fixture:"))
        {
          g_autofree char *fixture = bro_test_fixture_text (text + 8, NULL);
          if (fixture)
            add_lines (out->out, fixture);
        }
      else if (!strcmp (text, "saved") && written >= 0 && written != 1)
        {
          g_autoptr (GString) s = g_string_new (named);
          g_autofree char *count = g_strdup_printf ("Copy complete. %d titles", written);
          g_autofree char *quoted = g_strdup_printf (",\"%d\"", written);
          g_string_replace (s, "Copy complete. 1 titles", count, 0);
          g_string_replace (s, ",\"1\"", quoted, 0);
          g_ptr_array_add (out->out, g_strdup (s->str));
        }
      else
        g_ptr_array_add (out->out, fill (&f, named ? named : text, NULL));
    }
  v = J (rule, "stderr");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    g_ptr_array_add (out->err, fill (&f, bro_json_value_get_string (bro_json_value_at (v, i), ""), NULL));
  if ((v = J (rule, "probe")) && input && v->kind == BRO_JSON_VALUE_OBJECT)
    for (guint i = 0; i < v->keys->len; i++)
      if (g_pattern_match_simple (v->keys->pdata[i], name_of (input)))
        {
          BroJsonValue *answer = v->items->pdata[i];
          g_autoptr (BroJsonValue) none = bro_json_value_new_array ();
          g_ptr_array_add (out->out, probe_json (bro_json_value_get_number (J (answer, "durationSeconds"), 0),
                                                 J (answer, "tracks") ? J (answer, "tracks") : none,
                                                 (int) bro_json_value_get_integer (J (answer, "chapters"), 0)));
          break;
        }
  if (bro_json_value_get_bool (J (rule, "matchListing"), FALSE) && input)
    {
      g_autoptr (BroJsonValue) listed = bro_scenario_world_disc (self->world);
      if (listed)
        g_ptr_array_add (out->out, listed_probe (listed, name_of (input)));
    }

  v = J (rule, "exit");
  out->has_exit = !(v && v->kind == BRO_JSON_VALUE_STRING && !strcmp (v->string, "never"));
  out->exit = v && v->kind == BRO_JSON_VALUE_INTEGER ? (int) v->integer : 0;
  v = J (rule, "ignore");
  if (v)
    {
      out->ignore = g_ptr_array_new_with_free_func (g_free);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_ptr_array_add (out->ignore, g_strdup (bro_json_value_get_string (bro_json_value_at (v, i), "")));
    }
  else if (!strcmp (tool, "makemkvcon"))
    {
      out->ignore = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (out->ignore, g_strdup ("SIGINT"));
    }
  out->silence = bro_json_value_get_number (J (rule, "silence"), 0);
  args_clear (&mk_args);
}

static ScenarioProcess *
process_new (BroScenarioTools *self, const BroProcessSpec *spec, Plan *run)
{
  ScenarioProcess *p = g_object_new (TYPE_PROCESS, NULL);
  p->tools = g_object_ref (self);
  p->transcript = g_strdup (spec->transcript);
  p->executable = g_strdup (spec->executable);
  p->arguments = g_strdupv (spec->arguments);
  p->stop_policy = spec->stop_policy;
  p->has_stall = spec->has_stall_timeout;
  p->stall_seconds = spec->stall_timeout.seconds;
  p->run = *run;
  memset (run, 0, sizeof *run);
  return p;
}

static BroRunningProcess *
started (BroScenarioTools *self, ScenarioProcess *p)
{
  g_mutex_lock (&self->lock);
  g_ptr_array_add (self->alive, g_object_ref (p));
  g_mutex_unlock (&self->lock);
  begin (p);
  return BRO_RUNNING_PROCESS (p);
}

static BroRunningProcess *
tools_start (BroProcessLauncher *launcher, const BroProcessSpec *spec, BroBroError **error)
{
  BroScenarioTools *self = BRO_SCENARIO_TOOLS (launcher);
  const char *base = name_of (spec->executable);
  g_autofree char *tool = g_str_has_suffix (base, ".exe") ? g_strndup (base, strlen (base) - 4) : g_strdup (base);
  g_autofree char *point = NULL;
  g_autoptr (BroJsonValue) script = NULL;
  Plan run;
  ScenarioProcess *p;
  if (!strcmp (tool, "makemkvcon") && has_arg (spec->arguments, "disc:9999"))
    {
      g_auto (GStrv) scan = bro_scenario_world_drive_scan (self->world);
      plan_init (&run);
      for (guint i = 0; scan[i]; i++)
        g_ptr_array_add (run.out, g_strdup (scan[i]));
      run.exit = 0;
      return started (self, process_new (self, spec, &run));
    }
  {
    GString *call = g_string_new (tool);
    for (guint i = 0; spec->arguments && spec->arguments[i]; i++)
      g_string_append_printf (call, " %s", spec->arguments[i]);
    g_mutex_lock (&self->lock);
    g_ptr_array_add (self->calls, g_string_free (call, FALSE));
    if (self->step)
      point = g_strdup_printf ("inStep:%s:%d", self->step, ++self->in_step);
    g_mutex_unlock (&self->lock);
  }
  if (!(script = bro_scenario_world_script (self->world, tool)))
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "program", bro_json_value_new_string (spec->executable));
      bro_json_value_set (params, "reason", bro_json_value_new_string ("no script"));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_PROCESS_COULD_NOT_START), params);
      return NULL;
    }
  /* The process has started (its rule's files are written at its start) when the engine dies at the point. */
  plan_run (self, tool, spec, script, &run);
  if (point && self->crash && !self->crash (point, self->crash_data, error))
    {
      plan_clear (&run);
      return NULL;
    }
  p = process_new (self, spec, &run);
  return started (self, p);
}

static void
tools_iface_init (BroProcessLauncherInterface *iface)
{
  iface->start = tools_start;
}

static void
bro_scenario_tools_finalize (GObject *object)
{
  BroScenarioTools *self = BRO_SCENARIO_TOOLS (object);
  g_object_unref (self->clock);
  g_object_unref (self->fs);
  g_hash_table_unref (self->named);
  g_ptr_array_unref (self->calls);
  g_ptr_array_unref (self->alive);
  g_hash_table_unref (self->backups);
  g_free (self->step);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_scenario_tools_parent_class)->finalize (object);
}

static void
bro_scenario_tools_class_init (BroScenarioToolsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_scenario_tools_finalize;
}

static void
bro_scenario_tools_init (BroScenarioTools *self)
{
  g_mutex_init (&self->lock);
  self->calls = g_ptr_array_new_with_free_func (g_free);
  self->alive = g_ptr_array_new_with_free_func (g_object_unref);
  self->backups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_json_value_unref);
}

BroScenarioTools *
bro_scenario_tools_new (BroScenarioWorld *world, TestManualClock *clock, TestMemoryFs *fs)
{
  BroScenarioTools *self = g_object_new (BRO_TYPE_SCENARIO_TOOLS, NULL);
  self->world = world;
  self->clock = g_object_ref (clock);
  self->fs = g_object_ref (fs);
  self->named = bro_scenario_catalog_lines ();
  return self;
}

void
bro_scenario_tools_set_crash (BroScenarioTools *self, BroScenarioCrashFunc crash, gpointer data)
{
  self->crash = crash;
  self->crash_data = data;
}

void
bro_scenario_tools_step_begins (BroScenarioTools *self, const char *kind)
{
  g_mutex_lock (&self->lock);
  g_free (self->step);
  self->step = g_strdup (kind);
  self->in_step = 0;
  g_mutex_unlock (&self->lock);
}

GPtrArray *
bro_scenario_tools_calls (BroScenarioTools *self, const char *tool)
{
  GPtrArray *found = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *prefix = g_strconcat (tool, " ", NULL);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->calls->len; i++)
    {
      const char *c = self->calls->pdata[i];
      if (g_str_has_prefix (c, prefix) || !strcmp (c, tool))
        g_ptr_array_add (found, g_strdup (c));
    }
  g_mutex_unlock (&self->lock);
  return found;
}

int
bro_scenario_tools_parked (BroScenarioTools *self)
{
  int n = 0;
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->alive->len; i++)
    n += drained (self->alive->pdata[i]);
  g_mutex_unlock (&self->lock);
  return n;
}

void
bro_scenario_tools_end_all (BroScenarioTools *self)
{
  GPtrArray *alive;
  g_mutex_lock (&self->lock);
  alive = self->alive;
  self->alive = g_ptr_array_new_with_free_func (g_object_unref);
  g_mutex_unlock (&self->lock);
  for (guint i = 0; i < alive->len; i++)
    {
      BroProcessExit exit = { -1, 9, -1, FALSE, FALSE };
      end (alive->pdata[i], exit, TRUE);
    }
  g_ptr_array_unref (alive);
}
