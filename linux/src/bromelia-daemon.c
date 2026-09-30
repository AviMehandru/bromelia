/* bromelia-daemon.c — Bromelia without a window: watches the drives, rips discs as the configuration says and serves
 * the web page. For servers and containers (see docker/Dockerfile). */
#include "bro-state.h"

#include <glib-unix.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>

static GMainLoop *loop;
static GHashTable *last_states; /* job id -> BroJobState + 1 */
static gboolean stopping;

static void
say (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

static void
say (const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  printf ("%s %s\n", stamp, text);
  fflush (stdout);
}

/* One line per job state change. */
static void
on_jobs_changed (BroState *st)
{
  for (guint i = 0; i < st->jobs->len; i++)
    {
      BroJob *j = st->jobs->pdata[i];
      int before = GPOINTER_TO_INT (g_hash_table_lookup (last_states, j->id)) - 1;
      g_autofree char *title = NULL;
      if (before == (int) j->state)
        continue;
      g_hash_table_replace (last_states, g_strdup (j->id), GINT_TO_POINTER (j->state + 1));
      title = bro_job_title (j);
      if (bro_job_state_finished (j->state))
        say ("%s: %s%s%s%s%s", title, bro_job_state_label (j->state), j->output_dir ? " — " : "", j->output_dir ? j->output_dir : "",
             j->error ? " — " : "", j->error ? j->error : "");
      else
        say ("%s: %s (%s)", title, bro_job_state_label (j->state), bro_rip_mode_label (j->mode));
    }
  if (stopping && bro_state_active_count (st) == 0)
    g_main_loop_quit (loop);
}

static void
on_status_changed (BroState *st)
{
  static char *last;
  if (st->last_error && g_strcmp0 (st->last_error, last) != 0)
    say ("%s", st->last_error);
  g_free (last);
  last = g_strdup (st->last_error);
}

static gboolean
on_signal (gpointer data)
{
  BroState *st = data;
  if (stopping)
    {
      g_main_loop_quit (loop);
      return G_SOURCE_CONTINUE;
    }
  stopping = TRUE;
  say ("Stopping: cancelling running jobs (send the signal again to quit at once)");
  for (guint i = 0; i < st->jobs->len; i++)
    {
      BroJob *j = st->jobs->pdata[i];
      if (!bro_job_state_finished (j->state))
        bro_state_cancel_job (st, j);
    }
  if (bro_state_active_count (st) == 0)
    g_main_loop_quit (loop);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  g_autofree char *config = NULL, *listen = NULL, *token = NULL;
  int port = 0;
  gboolean version = FALSE;
  GOptionEntry entries[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &config, "Configuration file (default: ~/.config/bromelia/config.json)", "PATH" },
    { "listen", 'l', 0, G_OPTION_ARG_STRING, &listen, "Serve the web page on this address (0.0.0.0 = the network; needs a token)", "ADDRESS" },
    { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port of the web page (default 51280)", "PORT" },
    { "token", 't', 0, G_OPTION_ARG_STRING, &token, "Token for the web page (or BROMELIA_WEB_TOKEN)", "TOKEN" },
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Show the version", NULL },
    { NULL }
  };
  g_autoptr (GOptionContext) ctx = g_option_context_new ("— rip discs without a window");
  g_autoptr (GError) error = NULL;
  g_autoptr (BroState) st = NULL;
  g_autofree char *exe = NULL;

  setlocale (LC_ALL, "");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_set_description (ctx, "Drives, automatic rips, post-processing and notifications follow the configuration file, which "
                                         "the Bromelia app writes (or edit it by hand; see docs/configuration.md).");
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return 2;
    }
  if (version)
    {
      printf ("bromelia-daemon %s\n", PACKAGE_VERSION);
      return 0;
    }
  if (!token && g_getenv ("BROMELIA_WEB_TOKEN"))
    token = g_strdup (g_getenv ("BROMELIA_WEB_TOKEN"));

  st = bro_state_new_headless (config);
  if (listen || port || token)
    {
      BroWebUIConfig *w = &st->config->web_ui;
      /* The file keeps its own web settings (saved when the web page changes a setting). */
      static BroWebUIConfig saved;
      saved = *w;
      saved.address = g_strdup (w->address);
      saved.token = g_strdup (w->token);
      saved.tls_certificate = g_strdup (w->tls_certificate);
      saved.tls_key = g_strdup (w->tls_key);
      st->saved_web_ui = &saved;
      w->enabled = TRUE;
      if (listen)
        {
          g_free (w->address);
          w->address = g_strdup (listen);
        }
      if (port)
        w->port = port;
      if (token)
        {
          g_free (w->token);
          w->token = g_strdup (token);
        }
    }
  last_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  loop = g_main_loop_new (NULL, FALSE);
  g_signal_connect (st, "jobs-changed", G_CALLBACK (on_jobs_changed), NULL);
  g_signal_connect (st, "status-changed", G_CALLBACK (on_status_changed), NULL);
  g_unix_signal_add (SIGINT, on_signal, st);
  g_unix_signal_add (SIGTERM, on_signal, st);

  exe = bro_state_makemkvcon (st);
  say ("Bromelia %s; configuration %s", PACKAGE_VERSION, st->config_path);
  say ("makemkvcon: %s", exe ? exe : "not found — install MakeMKV or set makemkvconPath");
  say ("Output folder: %s", st->config->output_root);
  on_status_changed (st); /* e.g. jobs that were interrupted when it last stopped */
  bro_state_start (st);
  if (!st->owns_automation)
    {
      g_autofree char *holder = bro_state_automation_holder (st);
      say ("Another Bromelia (%s) rips inserted discs; this one takes over when it quits", holder);
    }
  if (st->config->web_ui.enabled)
    {
      const char *err = bro_state_web_error (st);
      if (err)
        say ("Web page: %s", err);
      else
        say ("Web page: %s://%s:%d/", st->config->web_ui.tls_certificate && *st->config->web_ui.tls_certificate ? "https" : "http",
             st->config->web_ui.address, st->config->web_ui.port);
    }
  g_main_loop_run (loop);
  /* Command-line web settings are not saved; history is saved as jobs finish. */
  g_main_loop_unref (loop);
  g_hash_table_unref (last_states);
  return 0;
}
