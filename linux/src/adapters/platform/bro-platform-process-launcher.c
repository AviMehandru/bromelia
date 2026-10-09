/* bro-platform-process-launcher.c */
#include "bro-platform-process-launcher.h"

#include "bro-argument-splitter.h"
#include "bro-message-code.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- the running process ---- */

#define BRO_TYPE_SPAWNED_PROCESS (bro_spawned_process_get_type ())
G_DECLARE_FINAL_TYPE (BroSpawnedProcess, bro_spawned_process, BRO, SPAWNED_PROCESS, GObject)

/* ---- output bytes into lines ---- */

#define MAX_LINE_BYTES 65536
#define CUT_MARK " [cut]"

/* Splits output as every launcher does: at \n, \r or \r\n, empty lines dropped, UTF-8 with bad bytes replaced. A line
 * is cut after MAX_LINE_BYTES bytes and ends with CUT_MARK; the rest of it, up to the next line break, is dropped, so a
 * tool writing binary can't grow memory without limit. */
typedef struct {
  GByteArray *pending;
  gboolean discarding;
} LineSplitter;

static char *
splitter_take (LineSplitter *s, gboolean cut)
{
  g_autofree char *text = g_utf8_make_valid ((const char *) s->pending->data, s->pending->len);
  g_byte_array_set_size (s->pending, 0);
  return cut ? g_strconcat (text, CUT_MARK, NULL) : g_steal_pointer (&text);
}

/* Appends the lines @bytes completes to @lines (char *). */
static void
splitter_feed (LineSplitter *s, const guint8 *bytes, gsize n, GPtrArray *lines)
{
  for (gsize i = 0; i < n; i++)
    {
      if (bytes[i] == '\n' || bytes[i] == '\r')
        {
          if (!s->discarding && s->pending->len > 0)
            g_ptr_array_add (lines, splitter_take (s, FALSE));
          g_byte_array_set_size (s->pending, 0);
          s->discarding = FALSE;
        }
      else if (!s->discarding)
        {
          g_byte_array_append (s->pending, bytes + i, 1);
          if (s->pending->len == MAX_LINE_BYTES)
            {
              g_ptr_array_add (lines, splitter_take (s, TRUE));
              s->discarding = TRUE;
            }
        }
    }
}

/* The last line, when the output didn't end with a line break; NULL otherwise. */
static char *
splitter_finish (LineSplitter *s)
{
  return !s->discarding && s->pending->len > 0 ? splitter_take (s, FALSE) : NULL;
}

struct _BroSpawnedProcess {
  GObject parent_instance;
  GPid pid;
  BroStopPolicy stop_policy;
  gboolean has_stall_timeout;
  double stall_seconds;
  GCancellable *read_cancel;
  GMutex lock;
  GCond cond;
  /* Guarded by lock: */
  char *transcript_path;               /* nullable */
  BroBroMessage *transcript_problem;   /* the first failure to open or write it; nullable */
  int transcript; /* fd, or -1: finish closes it while a reader may still be running, and the number may then belong to
                   * any file the engine opens */
  GQueue lines;          /* BroOutputLine *, waiting to be read */
  gsize queued_bytes;    /* the text of the lines in the queue */
  gboolean ended;        /* no more lines: the reader gets what is queued, then NULL */
  int waiting_for_room;  /* readers waiting for the queue: the tool isn't silent, nobody is reading */
  gint64 last_taken;     /* monotonic µs: when the reader last took a line */
  gint64 last_output; /* monotonic µs */
  int readers_left;
  gboolean exited;
  int wait_status;
  gint64 exited_at;
  gboolean stopping;
  BroStopReason reason;
  double stalled; /* < 0: didn't stall */
  gint64 stop_at, killed_at;
  int signals_sent;
  gboolean finished;
  BroProcessExit result;
  GPid watcher; /* its lifeline watcher, or 0 */
};

static void bro_spawned_process_iface_init (BroRunningProcessInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSpawnedProcess, bro_spawned_process, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_RUNNING_PROCESS, bro_spawned_process_iface_init))

static BroInstant
now_instant (void)
{
  return (BroInstant) { g_get_real_time () / 1000 };
}

/* 0, or the errno of a write that failed. */
static int
transcript_write (int fd, const char *line)
{
  g_autofree char *with_newline = NULL;
  size_t left;
  const char *p;
  if (fd < 0)
    return 0;
  with_newline = g_strconcat (line, "\n", NULL);
  /* One write per line with O_APPEND, under the lock: lines from the two readers never interleave. */
  p = with_newline;
  left = strlen (with_newline);
  while (left > 0)
    {
      ssize_t n = write (fd, p, left);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return n < 0 ? errno : EIO;
      p += n;
      left -= (size_t) n;
    }
  return 0;
}

static int
transcript_stamp (int fd, const char *what)
{
  g_autofree char *at = bro_instant_format (now_instant ());
  g_autofree char *line = g_strdup_printf ("==== %s %s", at, what);
  return transcript_write (fd, line);
}

static BroBroMessage *
no_transcript (const char *path, int code)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string (g_strerror (code)));
  return bro_bro_message_new (BRO_MSG_PROCESS_NO_TRANSCRIPT, params, BRO_SEVERITY_WARNING);
}

static BroOutputLine *
bro_spawned_process_lines (BroRunningProcess *process)
{
  BroSpawnedProcess *self = BRO_SPAWNED_PROCESS (process);
  BroOutputLine *line;
  g_mutex_lock (&self->lock);
  while (g_queue_is_empty (&self->lines) && !self->ended)
    g_cond_wait (&self->cond, &self->lock);
  line = g_queue_pop_head (&self->lines);
  if (line)
    {
      self->queued_bytes -= strlen (line->text);
      self->last_taken = g_get_monotonic_time ();
      g_cond_broadcast (&self->cond); /* room for a waiting reader */
    }
  g_mutex_unlock (&self->lock);
  return line;
}

static BroProcessExit
bro_spawned_process_wait (BroRunningProcess *process)
{
  BroSpawnedProcess *self = BRO_SPAWNED_PROCESS (process);
  BroProcessExit result;
  g_mutex_lock (&self->lock);
  while (!self->finished)
    g_cond_wait (&self->cond, &self->lock);
  result = self->result;
  g_mutex_unlock (&self->lock);
  return result;
}

/* With the lock held: the signals that are due, (INT →) TERM → KILL, 5 s apart. */
static void
escalate (BroSpawnedProcess *self, gint64 now)
{
  static const int interrupt_first[] = { SIGINT, SIGTERM, SIGKILL };
  static const int terminate_first[] = { SIGTERM, SIGKILL };
  const int *plan = self->stop_policy == BRO_STOP_POLICY_INTERRUPT_FIRST ? interrupt_first : terminate_first;
  int count = self->stop_policy == BRO_STOP_POLICY_INTERRUPT_FIRST ? 3 : 2;
  if (!self->stop_at || self->exited)
    return;
  while (self->signals_sent < count && now - self->stop_at >= (gint64) self->signals_sent * 5 * G_USEC_PER_SEC)
    {
      int sig = plan[self->signals_sent++];
      kill (-self->pid, sig);
      if (sig == SIGKILL)
        self->killed_at = now;
    }
}

/* With the lock held. */
static void
begin_stop (BroSpawnedProcess *self, BroStopReason reason)
{
  if (self->stopping)
    return;
  self->stopping = TRUE;
  self->reason = reason;
  if (self->exited)
    return;
  self->stop_at = g_get_monotonic_time ();
  escalate (self, self->stop_at);
}

static void
bro_spawned_process_stop (BroRunningProcess *process, BroStopReason reason)
{
  BroSpawnedProcess *self = BRO_SPAWNED_PROCESS (process);
  g_mutex_lock (&self->lock);
  begin_stop (self, reason);
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
}

static BroBroMessage *
bro_spawned_process_transcript_problem (BroRunningProcess *process)
{
  BroSpawnedProcess *self = BRO_SPAWNED_PROCESS (process);
  BroBroMessage *problem;
  g_mutex_lock (&self->lock);
  problem = self->transcript_problem ? bro_bro_message_copy (self->transcript_problem) : NULL;
  g_mutex_unlock (&self->lock);
  return problem;
}

typedef struct {
  BroSpawnedProcess *self;
  int fd;
  BroOutputSource source;
} Reader;

/* With the lock held: into the transcript, then the queue; when the queue is full, waits for room (a line after the end
 * is dropped). Takes @text. */
static void
emit (BroSpawnedProcess *self, BroOutputSource source, char *text)
{
  BroOutputLine *out;
  gboolean waited = FALSE;
  int failed = transcript_write (self->transcript, text);
  if (failed && !self->transcript_problem)
    self->transcript_problem = no_transcript (self->transcript_path, failed);
  while ((g_queue_get_length (&self->lines) >= BRO_QUEUED_LINES || self->queued_bytes >= BRO_QUEUED_BYTES) && !self->ended)
    {
      self->waiting_for_room++;
      g_cond_wait (&self->cond, &self->lock);
      self->waiting_for_room--;
      waited = TRUE;
    }
  if (waited)
    self->last_output = g_get_monotonic_time ();
  if (self->ended)
    {
      g_free (text);
      return;
    }
  out = bro_output_line_new ();
  out->stream = source;
  out->text = text;
  out->at = now_instant ();
  g_queue_push_tail (&self->lines, out);
  self->queued_bytes += strlen (out->text);
  g_cond_broadcast (&self->cond);
}

static gpointer
reader_thread (gpointer data)
{
  Reader *r = data;
  BroSpawnedProcess *self = r->self;
  g_autoptr (GInputStream) raw = g_unix_input_stream_new (r->fd, TRUE);
  g_autoptr (GPtrArray) lines = g_ptr_array_new ();
  LineSplitter splitter = { g_byte_array_new (), FALSE };
  guint8 buffer[65536];
  gssize n;
  char *last;
  while ((n = g_input_stream_read (raw, buffer, sizeof buffer, self->read_cancel, NULL)) > 0)
    {
      splitter_feed (&splitter, buffer, (gsize) n, lines);
      g_mutex_lock (&self->lock);
      self->last_output = g_get_monotonic_time ();
      for (guint i = 0; i < lines->len; i++)
        emit (self, r->source, lines->pdata[i]);
      g_mutex_unlock (&self->lock);
      g_ptr_array_set_size (lines, 0);
    }
  if ((last = splitter_finish (&splitter)) != NULL)
    {
      g_mutex_lock (&self->lock);
      emit (self, r->source, last);
      g_mutex_unlock (&self->lock);
    }
  g_byte_array_unref (splitter.pending);
  g_mutex_lock (&self->lock);
  self->readers_left--;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  g_object_unref (self);
  g_free (r);
  return NULL;
}

/* So that a crashed engine leaves no tools behind (as KILL_ON_JOB_CLOSE does on Windows), each process gets a watcher:
 * /bin/sh in its own process group, reading a pipe that nothing writes to. The engine holds the pipe's write end
 * (close-on-exec, never closed); when the engine ends, however it ends, the kernel closes it, the watcher's read
 * returns, and it stops the process's group: TERM, then KILL if the group is still there 5 s later. The watcher stops
 * as soon as the group is gone, so it never signals one used again. When the process ends first, its watcher is killed
 * and reaped before the process is reaped, so it can never signal a reused process group. (PR_SET_PDEATHSIG isn't
 * used: it fires when the thread that started the child ends, not the engine.) */
static const char lifeline_script[] = "read -r _; kill -s TERM -- \"-$1\" 2>/dev/null || exit 0; i=0; while [ $i -lt 50 ]; do sleep 0.1; "
                                      "kill -0 -- \"-$1\" 2>/dev/null || exit 0; i=$((i+1)); done; kill -s KILL -- \"-$1\" 2>/dev/null";

static void child_setup (gpointer data);

/* The pipe's read end; -1 when it couldn't be made. */
static int
lifeline_read_end (void)
{
  static gsize once;
  static int read_end = -1;
  if (g_once_init_enter (&once))
    {
      int fds[2];
      if (g_unix_open_pipe (fds, FD_CLOEXEC, NULL))
        read_end = fds[0]; /* fds[1] stays open until the engine ends */
      g_once_init_leave (&once, 1);
    }
  return read_end;
}

/* Starts the watcher of process group @group; 0 when it couldn't start (a crash then leaves the process running). */
static GPid
lifeline_watch (GPid group)
{
  int read_end = lifeline_read_end ();
  g_autofree char *arg = g_strdup_printf ("%d", (int) group);
  const char *argv[] = { "/bin/sh", "-c", lifeline_script, "bromelia-lifeline", arg, NULL };
  const char *envp[] = { "PATH=/usr/bin:/bin", NULL };
  GPid pid = 0;
  if (read_end < 0)
    return 0;
  if (!g_spawn_async_with_pipes_and_fds (NULL, argv, envp,
                                         G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                         child_setup, NULL, read_end, -1, -1, NULL, NULL, 0, &pid, NULL, NULL, NULL, NULL))
    return 0;
  return pid;
}

/* Kills and reaps a watcher whose process has ended. */
static void
lifeline_end (GPid watcher)
{
  int status;
  kill (watcher, SIGKILL);
  while (waitpid (watcher, &status, 0) == -1 && errno == EINTR)
    ;
}

/* Waits for the process to end without reaping it, so its pid (and process group) can't be reused while the
 * watchdog may still signal it; marks it ended; then reaps it. A process that never ends keeps this thread. */
static gpointer
waiter_thread (gpointer data)
{
  BroSpawnedProcess *self = data;
  siginfo_t info;
  int status = 0;
  while (waitid (P_PID, (id_t) self->pid, &info, WEXITED | WNOWAIT) == -1 && errno == EINTR)
    ;
  if (self->watcher > 0)
    lifeline_end (self->watcher); /* before the pid can be reused */
  g_mutex_lock (&self->lock);
  if (self->stopping)
    kill (-self->pid, SIGKILL); /* what is left of a stopped process's group */
  while (waitpid (self->pid, &status, 0) == -1 && errno == EINTR)
    ;
  self->exited = TRUE;
  self->wait_status = status;
  self->exited_at = g_get_monotonic_time ();
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  g_object_unref (self);
  return NULL;
}

/* With the lock held. */
static void
finish (BroSpawnedProcess *self, BroProcessExit exit)
{
  GString *ending = g_string_new (NULL);
  self->result = exit;
  self->finished = TRUE;
  g_cancellable_cancel (self->read_cancel);
  self->ended = TRUE; /* the reader gets what is queued, then NULL; readers waiting for room drop their lines */
  if (exit.abandoned)
    g_string_append (ending, "abandoned");
  else if (exit.signal)
    g_string_append_printf (ending, "signal %d", exit.signal);
  else
    g_string_append_printf (ending, "exit status %d", exit.status);
  if (exit.stalled_seconds >= 0)
    g_string_append (ending, ", stalled");
  if (exit.cancelled)
    g_string_append (ending, ", cancelled");
  {
    int failed = transcript_stamp (self->transcript, ending->str);
    if (failed && !self->transcript_problem)
      self->transcript_problem = no_transcript (self->transcript_path, failed);
  }
  g_string_free (ending, TRUE);
  if (self->transcript >= 0)
    close (self->transcript);
  self->transcript = -1;
  g_cond_broadcast (&self->cond);
}

static gpointer
watchdog_thread (gpointer data)
{
  BroSpawnedProcess *self = data;
  g_mutex_lock (&self->lock);
  while (!self->finished)
    {
      gint64 now = g_get_monotonic_time ();
      gboolean cancelled = self->stopping && (self->reason == BRO_STOP_REASON_CANCELLED || self->reason == BRO_STOP_REASON_SHUTDOWN);
      if (self->has_stall_timeout && !self->stopping && !self->exited
          && self->waiting_for_room == 0 && now - self->last_output >= (gint64) (self->stall_seconds * G_USEC_PER_SEC))
        {
          self->stalled = (double) ((now - self->last_output) / 1000) / 1000.0;
          begin_stop (self, BRO_STOP_REASON_STALLED);
        }
      escalate (self, now);
      if (!self->exited && self->killed_at && now - self->killed_at >= 30 * G_USEC_PER_SEC)
        {
          finish (self, (BroProcessExit) { -1, 0, self->stalled, TRUE, cancelled });
          break;
        }
      /* Output normally ends with the process; a child it left behind may keep a pipe open: stop reading 5 s after the
       * exit, unless lines are still waiting for a reader that keeps taking them. Queued lines count too: each take wakes
       * this thread as well, often while a reader is between two reads of the pipe, and ending then would drop what it
       * hasn't read yet. */
      if (self->exited
          && (self->readers_left == 0
              || (now - self->exited_at >= 5 * G_USEC_PER_SEC
                  && !((self->waiting_for_room > 0 || !g_queue_is_empty (&self->lines))
                       && now - self->last_taken < 5 * G_USEC_PER_SEC))))
        {
          BroProcessExit exit = { -1, 0, self->stalled, FALSE, cancelled };
          if (WIFEXITED (self->wait_status))
            exit.status = WEXITSTATUS (self->wait_status);
          else if (WIFSIGNALED (self->wait_status))
            exit.signal = WTERMSIG (self->wait_status);
          finish (self, exit);
          break;
        }
      g_cond_wait_until (&self->cond, &self->lock, now + G_USEC_PER_SEC / 4);
    }
  g_mutex_unlock (&self->lock);
  g_object_unref (self);
  return NULL;
}

static void
bro_spawned_process_finalize (GObject *object)
{
  BroSpawnedProcess *self = BRO_SPAWNED_PROCESS (object);
  gpointer item;
  while ((item = g_queue_pop_head (&self->lines)) != NULL)
    bro_output_line_free (item);
  g_clear_object (&self->read_cancel);
  g_free (self->transcript_path);
  g_clear_pointer (&self->transcript_problem, bro_bro_message_free);
  if (self->transcript >= 0)
    close (self->transcript);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (bro_spawned_process_parent_class)->finalize (object);
}

static void
bro_spawned_process_class_init (BroSpawnedProcessClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_spawned_process_finalize;
}

static void
bro_spawned_process_init (BroSpawnedProcess *self)
{
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  g_queue_init (&self->lines);
  self->last_taken = g_get_monotonic_time ();
  self->transcript = -1;
  self->stalled = -1;
  self->read_cancel = g_cancellable_new ();
  self->readers_left = 2;
}

static void
bro_spawned_process_iface_init (BroRunningProcessInterface *iface)
{
  iface->lines = bro_spawned_process_lines;
  iface->wait = bro_spawned_process_wait;
  iface->stop = bro_spawned_process_stop;
  iface->transcript_problem = bro_spawned_process_transcript_problem;
}

/* ---- the launcher ---- */

struct _BroPlatformProcessLauncher {
  GObject parent_instance;
};

static void bro_platform_process_launcher_iface_init (BroProcessLauncherInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformProcessLauncher, bro_platform_process_launcher, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_PROCESS_LAUNCHER, bro_platform_process_launcher_iface_init))

BroCommandLine *
bro_platform_process_launcher_invocation (const BroProcessSpec *spec)
{
  GPtrArray *args = g_ptr_array_new ();
  const char *exe = spec->executable;
  const char *program = exe;
  g_autofree char *interpreter = g_strstrip (g_strdup (spec->interpreter ? spec->interpreter : ""));
  if (*interpreter)
    program = interpreter;
  else if (g_file_test (exe, G_FILE_TEST_EXISTS) && !g_file_test (exe, G_FILE_TEST_IS_DIR) && access (exe, X_OK) != 0)
    program = "/bin/sh";
  if (program != exe)
    g_ptr_array_add (args, g_strdup (exe));
  for (guint i = 0; spec->arguments && spec->arguments[i]; i++)
    g_ptr_array_add (args, g_strdup (spec->arguments[i]));
  g_ptr_array_add (args, NULL);
  return bro_command_line_new (program, (GStrv) g_ptr_array_free (args, FALSE));
}

/* In the child, before exec: its own process group, default signal handling, nothing blocked. */
static void
child_setup (gpointer data)
{
  sigset_t none;
  setpgid (0, 0);
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
  signal (SIGPIPE, SIG_DFL);
  sigemptyset (&none);
  sigprocmask (SIG_SETMASK, &none, NULL);
}

static gboolean
could_not_start (BroBroError **error, int transcript, const char *program, const char *reason)
{
  g_autofree char *what = g_strdup_printf ("could not start: %s", reason);
  g_autofree char *tool = g_path_get_basename (program);
  BroJsonValue *params = bro_json_value_new_object ();
  transcript_stamp (transcript, what);
  if (transcript >= 0)
    close (transcript);
  bro_json_value_set (params, "tool", bro_json_value_new_string (tool));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_PROCESS_COULD_NOT_START), params);
  return FALSE;
}

BroRunningProcess *
bro_platform_process_launcher_start (BroPlatformProcessLauncher *self, const BroProcessSpec *spec, BroBroError **error)
{
  g_autoptr (BroCommandLine) cmd = bro_platform_process_launcher_invocation (spec);
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();
  g_autoptr (GString) shown = g_string_new (NULL);
  g_autoptr (GError) spawn_error = NULL;
  g_auto (GStrv) envp = g_get_environ ();
  GHashTableIter it;
  gpointer key, value;
  int transcript = -1, transcript_failed = 0, out_fd = -1, err_fd = -1;
  GPid pid = 0;
  BroSpawnedProcess *p;
  Reader *r;

  g_return_val_if_fail (BRO_IS_PLATFORM_PROCESS_LAUNCHER (self), NULL);
  g_ptr_array_add (argv, cmd->executable);
  for (guint i = 0; cmd->arguments[i]; i++)
    g_ptr_array_add (argv, cmd->arguments[i]);
  g_ptr_array_add (argv, NULL);
  for (guint i = 0; argv->pdata[i]; i++)
    {
      g_autofree char *q = bro_argument_splitter_quote (argv->pdata[i]);
      g_string_append_printf (shown, "%s%s", i ? " " : "$ ", q);
    }
  if (spec->transcript && *spec->transcript)
    {
      g_autofree char *dir = g_path_get_dirname (spec->transcript);
      g_mkdir_with_parents (dir, 0755);
      transcript = open (spec->transcript, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
      transcript_failed = transcript < 0 ? errno : 0;
    }
  if (!transcript_failed)
    transcript_failed = transcript_stamp (transcript, shown->str);

  if (spec->working_directory && *spec->working_directory && !g_file_test (spec->working_directory, G_FILE_TEST_IS_DIR))
    {
      g_autofree char *reason = g_strdup_printf ("the working folder %s doesn't exist", spec->working_directory);
      could_not_start (error, transcript, cmd->executable, reason);
      return NULL;
    }
  if (spec->environment)
    {
      g_hash_table_iter_init (&it, spec->environment);
      while (g_hash_table_iter_next (&it, &key, &value))
        envp = g_environ_setenv (envp, key, value, TRUE);
    }
  /* GLib's spawn rather than GSubprocess: GSubprocess reaps the child itself, and once it has, its pid may belong to
   * another process. Here the waiter thread reaps it, after the watchdog can no longer signal it. */
  if (!g_spawn_async_with_pipes_and_fds (spec->working_directory && *spec->working_directory ? spec->working_directory : NULL,
                                         (const char *const *) argv->pdata, (const char *const *) envp,
                                         G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL
                                           | G_SPAWN_CLOEXEC_PIPES,
                                         child_setup, NULL, -1, -1, -1, NULL, NULL, 0, &pid, NULL, &out_fd, &err_fd, &spawn_error))
    {
      could_not_start (error, transcript, cmd->executable, spawn_error->message);
      return NULL;
    }
  setpgid (pid, pid); /* also from here, so a stop straight away reaches the group */

  p = g_object_new (BRO_TYPE_SPAWNED_PROCESS, NULL);
  p->pid = pid;
  p->transcript = transcript;
  p->transcript_path = g_strdup (spec->transcript && *spec->transcript ? spec->transcript : NULL);
  if (transcript_failed)
    p->transcript_problem = no_transcript (spec->transcript, transcript_failed);
  p->stop_policy = spec->stop_policy;
  p->has_stall_timeout = spec->has_stall_timeout;
  p->stall_seconds = spec->stall_timeout.seconds;
  p->last_output = g_get_monotonic_time ();
  p->watcher = lifeline_watch (pid);

  r = g_new0 (Reader, 1);
  *r = (Reader) { g_object_ref (p), out_fd, BRO_OUTPUT_SOURCE_STDOUT };
  g_thread_unref (g_thread_new ("bro-process-out", reader_thread, r));
  r = g_new0 (Reader, 1);
  *r = (Reader) { g_object_ref (p), err_fd, BRO_OUTPUT_SOURCE_STDERR };
  g_thread_unref (g_thread_new ("bro-process-err", reader_thread, r));
  g_thread_unref (g_thread_new ("bro-process-wait", waiter_thread, g_object_ref (p)));
  g_thread_unref (g_thread_new ("bro-process-watch", watchdog_thread, g_object_ref (p)));
  return BRO_RUNNING_PROCESS (p);
}

static BroRunningProcess *
start_vfunc (BroProcessLauncher *self, const BroProcessSpec *spec, BroBroError **error)
{
  return bro_platform_process_launcher_start (BRO_PLATFORM_PROCESS_LAUNCHER (self), spec, error);
}

static void
bro_platform_process_launcher_class_init (BroPlatformProcessLauncherClass *klass)
{
}

static void
bro_platform_process_launcher_init (BroPlatformProcessLauncher *self)
{
}

static void
bro_platform_process_launcher_iface_init (BroProcessLauncherInterface *iface)
{
  iface->start = start_vfunc;
}

BroPlatformProcessLauncher *
bro_platform_process_launcher_new (void)
{
  return g_object_new (BRO_TYPE_PLATFORM_PROCESS_LAUNCHER, NULL);
}
