/* main.c — Bromelia for Linux (GTK 4 + libadwaita). */
#include "bro-pages.h"

#include <stdlib.h>
#include <string.h>

static void
load_css (void)
{
  g_autoptr (GtkCssProvider) css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/app/bromelia/Bromelia/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()), "/app/bromelia/Bromelia/icons");
}

static BroWindow *
ensure_window (AdwApplication *app)
{
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!win)
    win = GTK_WINDOW (bro_window_new (app));
  return BRO_WINDOW (win);
}

/* Developer aid: BROMELIA_SNAPSHOT=/path/shot.png renders the window to a PNG after
 * BROMELIA_SNAPSHOT_DELAY seconds (default 6) and quits. BROMELIA_SNAPSHOT_TAG selects a page
 * ("queue", "history", "drive:<id>", "source:<key>"); BROMELIA_SNAPSHOT_DIALOG=preferences|config opens a dialog, and BROMELIA_SNAPSHOT_PAGE=<name> one of its pages
 * ("services"; "general", "ripping", "output"… for config). */
static void
snapshot_after_paint (GdkFrameClock *clock, GtkWidget *win)
{
  const char *out = g_getenv ("BROMELIA_SNAPSHOT");
  int w = gtk_widget_get_width (win), h = gtk_widget_get_height (win);
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (win);
  GtkSnapshot *snap = gtk_snapshot_new ();
  g_autoptr (GskRenderNode) node = NULL;
  gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snap), w, h);
  node = gtk_snapshot_free_to_node (snap);
  if (!node)
    return; /* try again after the next frame */
  g_signal_handlers_disconnect_by_func (clock, snapshot_after_paint, win);
  {
    g_autoptr (GdkTexture) tex = gsk_renderer_render_texture (gtk_native_get_renderer (GTK_NATIVE (win)), node, &GRAPHENE_RECT_INIT (0, 0, w, h));
    if (!gdk_texture_save_to_png (tex, out))
      g_printerr ("snapshot: could not write %s\n", out);
  }
  g_application_quit (G_APPLICATION (gtk_window_get_application (GTK_WINDOW (win))));
}

static gboolean
take_snapshot (gpointer data)
{
  GtkWidget *win = data;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (win);
  g_signal_connect (clock, "after-paint", G_CALLBACK (snapshot_after_paint), win);
  gtk_widget_queue_draw (win);
  return G_SOURCE_REMOVE;
}

static gboolean
snapshot_redraw (gpointer data)
{
  gtk_window_present (GTK_WINDOW (data));
  gtk_widget_queue_draw (GTK_WIDGET (data));
  return G_SOURCE_REMOVE;
}

static gboolean
snapshot_prepare (gpointer data)
{
  BroWindow *win = data;
  const char *tag = g_getenv ("BROMELIA_SNAPSHOT_TAG");
  const char *dialog = g_getenv ("BROMELIA_SNAPSHOT_DIALOG");
  BroState *st = bro_app_state ();
  if (g_getenv ("BROMELIA_SNAPSHOT_RIP") && st->file_sessions->len)
    {
      /* Rip the second title of the first opened image into BROMELIA_SNAPSHOT_RIP (a folder). */
      BroSession *s = st->file_sessions->pdata[0];
      g_free (st->config->output_root);
      st->config->output_root = g_strdup (g_getenv ("BROMELIA_SNAPSHOT_RIP"));
      bro_session_select_all (s, FALSE);
      bro_session_set_selected (s, 1, TRUE);
      bro_state_rip_session (st, s, BRO_MODE_MKV);
    }
  if (tag && g_str_equal (tag, "first-source") && st->file_sessions->len)
    {
      g_autofree char *t = g_strdup_printf ("source:%s", ((BroSession *) st->file_sessions->pdata[0])->id);
      bro_window_navigate (win, t);
    }
  else if (tag && *tag)
    bro_window_navigate (win, tag);
  if (g_strcmp0 (dialog, "preferences") == 0)
    bro_preferences_present (GTK_WIDGET (win));
  else if (g_strcmp0 (dialog, "config") == 0)
    bro_config_dialog_present (GTK_WIDGET (win), st->config->default_drive->id);
  {
    const char *after = g_getenv ("BROMELIA_SNAPSHOT_AFTER");
    int secs = after ? atoi (after) : 2;
    g_timeout_add_seconds (MAX (1, secs - 1), snapshot_redraw, win);
    g_timeout_add_seconds (secs, take_snapshot, win);
  }
  return G_SOURCE_REMOVE;
}

static void
on_activate (AdwApplication *app, gpointer data)
{
  BroWindow *win = ensure_window (app);
  gtk_window_present (GTK_WINDOW (win));
  if (g_getenv ("BROMELIA_SNAPSHOT"))
    {
      const char *delay = g_getenv ("BROMELIA_SNAPSHOT_DELAY");
      g_timeout_add_seconds (delay ? atoi (delay) : 6, snapshot_prepare, win);
    }
}

static void
on_open (GApplication *app, GFile **files, int n, const char *hint, gpointer data)
{
  BroWindow *win = ensure_window (ADW_APPLICATION (app));
  for (int i = 0; i < n; i++)
    {
      g_autofree char *path = g_file_get_path (files[i]);
      if (path)
        {
          BroSession *s = bro_state_open_file (bro_app_state (), path);
          g_autofree char *tag = g_strdup_printf ("source:%s", s->id);
          bro_window_navigate (win, tag);
        }
    }
  gtk_window_present (GTK_WINDOW (win));
  if (g_getenv ("BROMELIA_SNAPSHOT"))
    {
      const char *delay = g_getenv ("BROMELIA_SNAPSHOT_DELAY");
      g_timeout_add_seconds (delay ? atoi (delay) : 6, snapshot_prepare, win);
    }
}

/* Keeps the computer from suspending (and the session from going idle) through the desktop, which also works in a
 * sandbox; the state decides when (jobs or background steps running, preventSleep on). */
typedef struct {
  BroSleepInhibitor base;
  GtkApplication *app;
  guint cookie;
} GtkInhibitor;

static void
gtk_inhibitor_set (BroSleepInhibitor *base, gboolean on)
{
  GtkInhibitor *self = (GtkInhibitor *) base;
  if (on && !self->cookie)
    self->cookie = gtk_application_inhibit (self->app, gtk_application_get_active_window (self->app),
                                            GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_IDLE, "Ripping discs");
  else if (!on && self->cookie)
    {
      gtk_application_uninhibit (self->app, self->cookie);
      self->cookie = 0;
    }
}

static void
gtk_inhibitor_free (BroSleepInhibitor *base)
{
  g_free (base);
}

static void
on_startup (GApplication *app, gpointer data)
{
  static const char *quit_accels[] = { "<Ctrl>q", NULL };
  static const char *open_accels[] = { "<Ctrl>o", NULL };
  static const char *rescan_accels[] = { "<Ctrl>r", NULL };
  static const char *prefs_accels[] = { "<Ctrl>comma", NULL };
  BroState *state;
  load_css ();
  state = bro_state_new (app);
  bro_app_set_state (state);
  {
    GtkInhibitor *inhibitor = g_new0 (GtkInhibitor, 1);
    inhibitor->base.set = gtk_inhibitor_set;
    inhibitor->base.free = gtk_inhibitor_free;
    inhibitor->app = GTK_APPLICATION (app);
    bro_state_set_sleep_inhibitor (state, &inhibitor->base);
  }
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.quit", quit_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.open", open_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.rescan", rescan_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.preferences", prefs_accels);
  bro_state_start (state);
}

static void
on_shutdown (GApplication *app, gpointer data)
{
  BroState *st = bro_app_state ();
  if (!st)
    return;
  for (guint i = 0; i < st->jobs->len; i++)
    {
      BroJob *j = st->jobs->pdata[i];
      if (j->state == BRO_JOB_RUNNING && j->cancellable)
        g_cancellable_cancel (j->cancellable);
    }
  bro_state_save_now (st);
}

static AdwDialog *quit_dialog; /* the open "still running" alert, so a second Ctrl+Q or close doesn't stack another */

static void
quit_response (AdwAlertDialog *d, const char *response, GApplication *app)
{
  if (g_str_equal (response, "quit"))
    g_application_quit (app);
}

static void
quit_dialog_closed (AdwDialog *d, gpointer data)
{
  quit_dialog = NULL;
}

/* app.quit — Ctrl+Q, and closing the main window while something is running (bro-window.c). */
static void
action_quit (GSimpleAction *a, GVariant *p, gpointer app)
{
  BroState *st = bro_app_state ();
  int running = st ? bro_state_active_count (st) : 0;
  gboolean background = st && bro_state_background_busy (st);
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  g_autofree char *title = NULL;
  const char *body;
  if ((running == 0 && !background) || !win)
    {
      g_application_quit (app);
      return;
    }
  if (quit_dialog)
    return;
  if (running > 0)
    {
      title = g_strdup_printf ("%d job(s) are still running", running);
      body = background ? "Quitting cancels them and abandons post-processing that hasn't finished. Partially written files are left in place."
                        : "Quitting cancels them. Partially written files are left in place.";
    }
  else
    {
      title = g_strdup ("Post-processing is still running");
      body = "Quitting abandons the steps that haven't finished; they are not resumed at the next start.";
    }
  quit_dialog = adw_alert_dialog_new (title, body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (quit_dialog), "keep", "Keep Running", "quit", "Cancel Jobs and Quit", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (quit_dialog), "quit", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (quit_dialog), "keep");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (quit_dialog), "keep");
  g_signal_connect (quit_dialog, "response", G_CALLBACK (quit_response), app);
  g_signal_connect (quit_dialog, "closed", G_CALLBACK (quit_dialog_closed), NULL);
  adw_dialog_present (quit_dialog, GTK_WIDGET (win));
}

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = adw_application_new (APP_ID, G_APPLICATION_HANDLES_OPEN);
  static const GActionEntry app_actions[] = { { "quit", action_quit, NULL, NULL, NULL, { 0 } } };
  g_set_application_name ("Bromelia");
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), app);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
