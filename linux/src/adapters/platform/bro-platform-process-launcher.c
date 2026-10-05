/* bro-platform-process-launcher.c */
#include "bro-platform-process-launcher.h"

#include "bro-argument-splitter.h"
#include "bro-message-code.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- the running process ---- */

#define BRO_TYPE_SPAWNED_PROCESS (bro_spawned_process_get_type ())
G_DECLARE_FINAL_TYPE (BroSpawnedProcess, bro_spawned_process, BRO, SPAWNED_PROCESS, GObject)

/* Marks the end of the line queue. */
static char end_of_lines;

struct _BroSpawnedProcess {
  GObject parent_instance;
  GPid pid;
  BroStopPolicy stop_policy;
  gboolean has_stall_timeout;
  double stall_seconds;
  GAsyncQueue *lines; /* BroOutputLine *, then &end_of_lines */
  GCancellable *read_cancel;
  GMutex lock;
  GCond cond;
  /* Guarded by lock: */
  int transcript; /* fd, or -1: finish closes it while a reader may still be running, and the number may then belong to
                   * any file the engine opens */
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
};

static void bro_spawned_process_iface_init (BroRunningProcessInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSpawnedProcess, bro_spawned_process, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_RUNNING_PROCESS, bro_spawned_process_iface_init))

static BroInstant
now_instant (void)
{
  return (BroInstant) { g_get_real_time () / 1000 };
}

static void
transcript_write (int fd, const char *line)
{
  g_autofree char *with_newline = NULL;
  size_t left;
  const char *p;
  if (fd < 0)
    return;
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
        return;
      p += n;
      left -= (size_t) n;
    }
}

static void
transcript_stamp (int fd, const char *what)
{
  g_autofree char *at = bro_instant_format (now_instant ());
  g_autofree char *line = g_strdup_printf ("==== %s %s", at, what);
  transcript_write (fd, line);
}

static BroOutputLine *
bro_spawned_process_lines (BroRunningProcess *process)
{
  BroSpawnedProcess *self = BRO_SPAWNED_PROCESS (process);
  gpointer item = g_async_queue_pop (self->lines);
  if (item == &end_of_lines)
    {
      g_async_queue_push (self->lines, &end_of_lines); /* every later call ends too */
      return NULL;
    }
  return item;
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

typedef struct {
  BroSpawnedProcess *self;
  int fd;
  BroOutputSource source;
} Reader;

static gpointer
reader_thread (gpointer data)
{
  Reader *r = data;
  BroSpawnedProcess *self = r->self;
  g_autoptr (GInputStream) raw = g_unix_input_stream_new (r->fd, TRUE);
  g_autoptr (GDataInputStream) in = g_data_input_stream_new (raw);
  char *line;
  g_data_input_stream_set_newline_type (in, G_DATA_STREAM_NEWLINE_TYPE_ANY);
  while ((line = g_data_input_stream_read_line (in, NULL, self->read_cancel, NULL)) != NULL)
    {
      g_autofree char *valid = g_utf8_make_valid (line, -1);
      g_free (line);
      g_mutex_lock (&self->lock);
      self->last_output = g_get_monotonic_time ();
      if (*valid)
        transcript_write (self->transcript, valid);
      g_mutex_unlock (&self->lock);
      if (*valid)
        {
          BroOutputLine *out = bro_output_line_new ();
          out->stream = r->source;
          out->text = g_steal_pointer (&valid);
          out->at = now_instant ();
          g_async_queue_push (self->lines, out);
        }
    }
  g_mutex_lock (&self->lock);
  self->readers_left--;
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
  g_object_unref (self);
  g_free (r);
  return NULL;
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
  g_async_queue_push (self->lines, &end_of_lines);
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
  transcript_stamp (self->transcript, ending->str);
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
          && now - self->last_output >= (gint64) (self->stall_seconds * G_USEC_PER_SEC))
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
      /* Output normally ends with the process; a child it left behind may keep a pipe open. */
      if (self->exited && (self->readers_left == 0 || now - self->exited_at >= 5 * G_USEC_PER_SEC))
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
  while ((item = g_async_queue_try_pop (self->lines)) != NULL)
    if (item != &end_of_lines)
      bro_output_line_free (item);
  g_async_queue_unref (self->lines);
  g_clear_object (&self->read_cancel);
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
  self->lines = g_async_queue_new ();
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
  int transcript = -1, out_fd = -1, err_fd = -1;
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
    }
  transcript_stamp (transcript, shown->str);

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
  p->stop_policy = spec->stop_policy;
  p->has_stall_timeout = spec->has_stall_timeout;
  p->stall_seconds = spec->stall_timeout.seconds;
  p->last_output = g_get_monotonic_time ();

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
