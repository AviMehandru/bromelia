/* bro-pages.c — queue, history and drive tools pages. */
#include "bro-pages.h"
#include "bro-identity.h"
#include "bro-logic.h"

/* ---- queue ---- */

static void
on_clear_background (GtkButton *b, gpointer data)
{
  bro_state_clear_background (bro_app_state ());
}

/* Background post-processing (encoding, uploads) of finished jobs. */
static GtkWidget *
background_list (void)
{
  BroState *st = bro_app_state ();
  GtkWidget *group = adw_preferences_group_new ();
  GtkWidget *clear = gtk_button_new_with_label ("Clear Finished");
  gtk_widget_add_css_class (clear, "flat");
  g_signal_connect (clear, "clicked", G_CALLBACK (on_clear_background), NULL);
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), "Background");
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (group), clear);
  for (guint i = 0; i < st->background->len; i++)
    {
      BroBackgroundItem *b = st->background->pdata[i];
      GtkWidget *row = adw_action_row_new ();
      GtkWidget *icon = gtk_image_new_from_icon_name (g_str_equal (b->state, "done") ? "emblem-ok-symbolic"
                                                      : g_str_equal (b->state, "failed") ? "dialog-error-symbolic"
                                                      : g_str_equal (b->state, "running") ? "system-run-symbolic" : "alarm-symbolic");
      g_autofree char *title = g_markup_escape_text (b->work->title, -1);
      g_autofree char *sub = g_markup_escape_text (*b->message ? b->message : b->state, -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), icon);
      if (g_str_equal (b->state, "failed"))
        gtk_widget_add_css_class (icon, "error");
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
    }
  return group;
}

static void
queue_rebuild (GtkWidget *box)
{
  BroState *st = bro_app_state ();
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), c);
  if (st->background->len)
    gtk_box_append (GTK_BOX (box), background_list ());
  if (st->jobs->len == 0 && st->background->len == 0)
    {
      GtkWidget *empty = adw_status_page_new ();
      adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "view-list-symbolic");
      adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No jobs");
      adw_status_page_set_description (ADW_STATUS_PAGE (empty),
                                       "Insert a disc and choose Rip, or open a disc to pick titles. Drives set to rip automatically start jobs on their own.");
      gtk_widget_set_vexpand (empty, TRUE);
      gtk_box_append (GTK_BOX (box), empty);
      return;
    }
  for (guint i = 0; i < st->jobs->len; i++)
    gtk_box_append (GTK_BOX (box), bro_job_card_new (st->jobs->pdata[i]));
}

static void
on_clear_finished (GtkButton *b, gpointer data)
{
  bro_state_clear_finished (bro_app_state ());
}

GtkWidget *
bro_queue_page_new (void)
{
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *title = gtk_label_new ("Queue");
  GtkWidget *clear = gtk_button_new_with_label ("Clear Finished");
  GtkWidget *scroll = gtk_scrolled_window_new ();
  GtkWidget *clamp = adw_clamp_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_add_css_class (title, "title-2");
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_clear_finished), NULL);
  gtk_box_append (GTK_BOX (bar), title);
  gtk_box_append (GTK_BOX (bar), clear);
  gtk_widget_set_margin_start (bar, 18);
  gtk_widget_set_margin_end (bar, 18);
  gtk_widget_set_margin_top (bar, 12);

  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 1100);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);
  adw_clamp_set_child (ADW_CLAMP (clamp), box);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (outer), bar);
  gtk_box_append (GTK_BOX (outer), scroll);

  g_signal_connect_object (bro_app_state (), "jobs-changed", G_CALLBACK (queue_rebuild), box, G_CONNECT_SWAPPED);
  queue_rebuild (box);
  return outer;
}

/* ---- history ---- */

typedef struct {
  GtkWidget *list;
  GtkWidget *detail;
} History;

static void
history_open_folder (GtkButton *b, const char *dir)
{
  bro_open_path (GTK_WIDGET (b), dir);
}

static void
history_open_log (GtkButton *b, const char *path)
{
  bro_open_path (GTK_WIDGET (b), path);
}

/* Re-checks a finished job's folder against its SHA256SUMS, on the Archive Check page. */
static void
history_verify (GtkButton *b, const char *dir)
{
  GtkRoot *win = gtk_widget_get_root (GTK_WIDGET (b));
  if (!bro_state_verify_start (bro_app_state (), dir, FALSE))
    bro_state_set_error (bro_app_state (), "An archive check is already running");
  if (BRO_IS_WINDOW (win))
    bro_window_navigate (BRO_WINDOW (win), "verify");
}

static void
history_show (History *h, BroHistoryRecord *r)
{
  GtkWidget *c;
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  g_autofree char *log = NULL;
  GtkWidget *view, *scroll;
  while ((c = gtk_widget_get_first_child (h->detail)))
    gtk_box_remove (GTK_BOX (h->detail), c);
  {
    GtkWidget *t = gtk_label_new (r->title);
    gtk_widget_add_css_class (t, "heading");
    gtk_label_set_xalign (GTK_LABEL (t), 0);
    gtk_label_set_wrap (GTK_LABEL (t), TRUE);
    gtk_box_append (GTK_BOX (h->detail), t);
  }
  if (r->error && *r->error)
    {
      GtkWidget *e = gtk_label_new (r->error);
      gtk_widget_add_css_class (e, "error");
      gtk_label_set_wrap (GTK_LABEL (e), TRUE);
      gtk_label_set_xalign (GTK_LABEL (e), 0);
      gtk_box_append (GTK_BOX (h->detail), e);
    }
  if (r->output_dir && *r->output_dir)
    {
      GtkWidget *b = gtk_button_new_with_label ("Open Output Folder");
      g_autofree char *sums = g_build_filename (r->output_dir, BRO_CHECKSUM_FILE, NULL);
      BroCheckRecord *rec = bro_state_check_record (bro_app_state (), r->output_dir);
      g_signal_connect_data (b, "clicked", G_CALLBACK (history_open_folder), g_strdup (r->output_dir), bro_closure_free, 0);
      gtk_box_append (GTK_BOX (buttons), b);
      if (g_file_test (sums, G_FILE_TEST_EXISTS))
        {
          g_autofree char *checked = NULL;
          b = gtk_button_new_with_label ("Verify Folder");
          gtk_widget_set_tooltip_text (b, "Read the files again and compare them with SHA256SUMS");
          g_signal_connect_data (b, "clicked", G_CALLBACK (history_verify), g_strdup (r->output_dir), bro_closure_free, 0);
          gtk_box_append (GTK_BOX (buttons), b);
          if (rec)
            {
              g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (rec->checked_at);
              g_autofree char *when = g_date_time_format (d, "%x %X");
              checked = g_strdup_printf ("Last verified %s: %s", when, rec->summary);
            }
          else
            checked = g_strdup ("Not verified since it was archived");
          gtk_box_append (GTK_BOX (h->detail), bro_caption (checked));
        }
    }
  {
    GtkWidget *b = gtk_button_new_with_label ("Open Log File");
    g_signal_connect_data (b, "clicked", G_CALLBACK (history_open_log), g_strdup (r->log_path), bro_closure_free, 0);
    gtk_box_append (GTK_BOX (buttons), b);
  }
  gtk_box_append (GTK_BOX (h->detail), buttons);
  for (guint i = 0; i < r->files->len; i++)
    gtk_box_append (GTK_BOX (h->detail), bro_caption (r->files->pdata[i]));
  g_file_get_contents (r->log_path, &log, NULL, NULL);
  view = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_widget_add_css_class (view, "log-view");
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), log ? log : "Log not available", -1);
  scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), view);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_add_css_class (scroll, "card");
  gtk_box_append (GTK_BOX (h->detail), scroll);
}

static void
history_row_activated (GtkListBox *box, GtkListBoxRow *row, History *h)
{
  const char *id = g_object_get_data (G_OBJECT (row), "id");
  BroState *st = bro_app_state ();
  for (guint i = 0; i < st->history->len; i++)
    {
      BroHistoryRecord *r = st->history->pdata[i];
      if (g_strcmp0 (r->id, id) == 0)
        history_show (h, r);
    }
}

static void
history_rebuild (History *h)
{
  BroState *st = bro_app_state ();
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (h->list)))
    gtk_list_box_remove (GTK_LIST_BOX (h->list), c);
  for (guint i = 0; i < st->history->len; i++)
    {
      BroHistoryRecord *r = st->history->pdata[i];
      GtkWidget *row = adw_action_row_new ();
      g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (r->finished_at ? r->finished_at : r->started_at);
      g_autofree char *when = dt ? g_date_time_format (dt, "%x %X") : g_strdup ("");
      g_autofree char *dur = r->started_at && r->finished_at ? bro_format_elapsed (r->finished_at - r->started_at) : g_strdup ("");
      g_autofree char *title = g_markup_escape_text (r->title ? r->title : "", -1);
      BroCheckRecord *rec = r->output_dir ? bro_state_check_record (st, r->output_dir) : NULL;
      g_autofree char *sub = g_strdup_printf ("%s · %s · %s · %u item(s) · %s%s", when, r->mode, r->state, r->files->len, dur,
                                              !rec ? "" : rec->ok ? " · verified OK" : " · DAMAGED (verify)");
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row),
                                 gtk_image_new_from_icon_name (g_strcmp0 (r->state, "success") == 0 ? "emblem-ok-symbolic"
                                                               : g_strcmp0 (r->state, "errors") == 0 ? "dialog-warning-symbolic"
                                                               : "dialog-error-symbolic"));
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_object_set_data_full (G_OBJECT (row), "id", g_strdup (r->id), g_free);
      gtk_list_box_append (GTK_LIST_BOX (h->list), row);
    }
}

static void
history_rebuild_widget (GtkWidget *outer)
{
  history_rebuild (g_object_get_data (G_OBJECT (outer), "history"));
}

static void
clear_history_response (AdwAlertDialog *d, const char *response, gpointer data)
{
  if (g_str_equal (response, "clear"))
    bro_state_clear_history (bro_app_state ());
}

static void
on_clear_history (GtkButton *b, gpointer data)
{
  AdwDialog *d = adw_alert_dialog_new ("Clear the job history?", "Stored job logs are deleted as well.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", "Cancel", "clear", "Clear", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "clear", ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect (d, "response", G_CALLBACK (clear_history_response), NULL);
  adw_dialog_present (d, GTK_WIDGET (b));
}

GtkWidget *
bro_history_page_new (void)
{
  History *h = g_new0 (History, 1);
  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *title = gtk_label_new ("History");
  GtkWidget *clear = gtk_button_new_with_label ("Clear History…");
  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  GtkWidget *scroll = gtk_scrolled_window_new ();

  gtk_widget_add_css_class (title, "title-2");
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_clear_history), NULL);
  gtk_box_append (GTK_BOX (bar), title);
  gtk_box_append (GTK_BOX (bar), clear);

  h->list = gtk_list_box_new ();
  gtk_widget_add_css_class (h->list, "boxed-list");
  gtk_widget_set_valign (h->list, GTK_ALIGN_START);
  g_signal_connect (h->list, "row-activated", G_CALLBACK (history_row_activated), h);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), h->list);
  h->detail = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request (h->detail, -1, 200);
  gtk_paned_set_start_child (GTK_PANED (paned), scroll);
  gtk_paned_set_end_child (GTK_PANED (paned), h->detail);
  gtk_paned_set_position (GTK_PANED (paned), 380);
  gtk_widget_set_vexpand (paned, TRUE);

  gtk_box_append (GTK_BOX (outer), bar);
  gtk_box_append (GTK_BOX (outer), paned);
  gtk_widget_set_margin_start (outer, 18);
  gtk_widget_set_margin_end (outer, 18);
  gtk_widget_set_margin_top (outer, 12);
  gtk_widget_set_margin_bottom (outer, 12);
  g_object_set_data_full (G_OBJECT (outer), "history", h, g_free);
  g_signal_connect_object (bro_app_state (), "history-changed", G_CALLBACK (history_rebuild_widget), outer, G_CONNECT_SWAPPED);
  history_rebuild (h);
  return outer;
}

/* ---- drive tools ---- */

typedef struct {
  GtkWidget *output;
  GtkWidget *drive;
  GtkWidget *root;
} Tools;

typedef struct {
  char **argv;
  GString *out;
  int status;
} ToolRun;

static void
tool_line (const char *line, gpointer data)
{
  g_string_append_printf (((ToolRun *) data)->out, "%s\n", line);
}

static void
tool_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  ToolRun *r = data;
  bro_process_run ((const char *const *) r->argv, NULL, NULL, 120, NULL, tool_line, r, &r->status, NULL, NULL, NULL);
  g_task_return_boolean (task, TRUE);
}

static void
tool_run_free (ToolRun *r)
{
  g_strfreev (r->argv);
  g_string_free (r->out, TRUE);
  g_free (r);
}

static void
tool_done (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *view = data;
  ToolRun *r = g_task_get_task_data (G_TASK (res));
  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter end;
  g_autofree char *tail = g_strdup_printf ("%s\n(exit status %d)\n", r->out->str, r->status);
  gtk_text_buffer_get_end_iter (buf, &end);
  gtk_text_buffer_insert (buf, &end, tail, -1);
  g_object_unref (view);
}

static void
tools_run (Tools *t, const char *const *args)
{
  g_autofree char *exe = bro_state_makemkvcon (bro_app_state ());
  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (t->output));
  ToolRun *r;
  GTask *task;
  GPtrArray *argv = g_ptr_array_new ();
  if (!exe)
    {
      gtk_text_buffer_set_text (buf, "makemkvcon not found", -1);
      return;
    }
  g_ptr_array_add (argv, g_strdup (exe));
  for (int i = 0; args[i]; i++)
    g_ptr_array_add (argv, g_strdup (args[i]));
  g_ptr_array_add (argv, NULL);
  r = g_new0 (ToolRun, 1);
  r->argv = (char **) g_ptr_array_free (argv, FALSE);
  r->out = g_string_new (NULL);
  {
    g_autofree char *cmd = bro_command_line ((const char *const *) r->argv);
    g_autofree char *head = g_strdup_printf ("$ %s\n", cmd);
    gtk_text_buffer_set_text (buf, head, -1);
  }
  task = g_task_new (NULL, NULL, tool_done, g_object_ref (t->output));
  g_task_set_task_data (task, r, (GDestroyNotify) tool_run_free);
  g_task_run_in_thread (task, tool_thread);
  g_object_unref (task);
}

static void
on_list_drives (GtkButton *b, Tools *t)
{
  const char *args[] = { "f", "-l", NULL };
  tools_run (t, args);
}

static void
on_drive_help (GtkButton *b, Tools *t)
{
  const char *drive = gtk_editable_get_text (GTK_EDITABLE (t->drive));
  const char *args[] = { "f", "-d", drive, "help", NULL };
  if (*drive)
    tools_run (t, args);
}

static void
on_sdf_info (GtkButton *b, Tools *t)
{
  const char *args[] = { "f", "--info", NULL };
  tools_run (t, args);
}

GtkWidget *
bro_tools_page_new (void)
{
  Tools *t = g_new0 (Tools, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *list = gtk_button_new_with_label ("List Drives");
  GtkWidget *help = gtk_button_new_with_label ("Drive Commands");
  GtkWidget *info = gtk_button_new_with_label ("SDF Info");
  GtkWidget *scroll = gtk_scrolled_window_new ();
  BroState *st = bro_app_state ();

  gtk_box_append (GTK_BOX (box), bro_caption ("Uses MakeMKV's firmware utility (makemkvcon f). Only read-only commands are offered; "
                                              "flashing firmware must be done from the command line."));
  t->drive = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (t->drive), "Drive (device, e.g. /dev/sr0)");
  gtk_widget_set_hexpand (t->drive, TRUE);
  if (st->drives->len)
    gtk_editable_set_text (GTK_EDITABLE (t->drive), ((BroDriveEntry *) st->drives->pdata[0])->device);
  g_signal_connect (list, "clicked", G_CALLBACK (on_list_drives), t);
  g_signal_connect (help, "clicked", G_CALLBACK (on_drive_help), t);
  g_signal_connect (info, "clicked", G_CALLBACK (on_sdf_info), t);
  gtk_box_append (GTK_BOX (bar), list);
  gtk_box_append (GTK_BOX (bar), t->drive);
  gtk_box_append (GTK_BOX (bar), help);
  gtk_box_append (GTK_BOX (bar), info);
  gtk_box_append (GTK_BOX (box), bar);
  t->output = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (t->output), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (t->output), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), t->output);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_add_css_class (scroll, "card");
  gtk_box_append (GTK_BOX (box), scroll);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  g_object_set_data_full (G_OBJECT (box), "tools", t, g_free);
  return box;
}

/* ---- archive check ---- */

typedef struct {
  GtkWidget *root, *last, *progress, *progress_label, *cancel, *check_root, *check_folder, *results, *results_title;
  gboolean shown_running; /* the results list shows the last finished run */
} Verify;

static char *
verify_date (gint64 when)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (when);
  return g_date_time_format (d, "%x %X");
}

static void
verify_open_folder (GtkButton *b, const char *dir)
{
  bro_open_path (GTK_WIDGET (b), dir);
}

/* The files of one category, at most 50. */
static void
verify_add_files (AdwExpanderRow *row, const char *what, GPtrArray *files)
{
  for (guint i = 0; i < files->len && i < 50; i++)
    {
      GtkWidget *r = adw_action_row_new ();
      g_autofree char *t = g_markup_escape_text (files->pdata[i], -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r), t);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (r), what);
      adw_expander_row_add_row (row, r);
    }
  if (files->len > 50)
    {
      GtkWidget *r = adw_action_row_new ();
      g_autofree char *t = g_strdup_printf ("… and %u more %s", files->len - 50, what);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r), t);
      adw_expander_row_add_row (row, r);
    }
}

static void
verify_rebuild_results (Verify *v)
{
  BroVerifyStatus *s = &bro_app_state ()->verify;
  GtkWidget *c;
  int damaged = 0;
  while ((c = gtk_widget_get_first_child (v->results)))
    gtk_list_box_remove (GTK_LIST_BOX (v->results), c);
  for (guint i = 0; s->results && i < s->results->len; i++)
    {
      BroFolderCheck *r = s->results->pdata[i];
      gboolean ok = bro_folder_check_ok (r);
      GtkWidget *row = adw_expander_row_new ();
      g_autofree char *base = g_path_get_basename (r->folder);
      g_autofree char *title = g_markup_escape_text (base, -1);
      g_autofree char *summary = bro_folder_check_summary (r);
      g_autofree char *sub = g_markup_printf_escaped ("%s\n%s", r->folder, summary);
      GtkWidget *open = gtk_button_new_from_icon_name ("folder-open-symbolic");
      damaged += !ok;
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (row), sub);
      adw_expander_row_add_prefix (ADW_EXPANDER_ROW (row),
                                   gtk_image_new_from_icon_name (ok ? "emblem-ok-symbolic" : "dialog-error-symbolic"));
      gtk_widget_set_valign (open, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (open, "flat");
      gtk_widget_set_tooltip_text (open, "Open the folder");
      g_signal_connect_data (open, "clicked", G_CALLBACK (verify_open_folder), g_strdup (r->folder), bro_closure_free, 0);
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (row), open);
      verify_add_files (ADW_EXPANDER_ROW (row), "changed", r->changed);
      verify_add_files (ADW_EXPANDER_ROW (row), "unreadable", r->unreadable);
      verify_add_files (ADW_EXPANDER_ROW (row), "missing", r->missing);
      verify_add_files (ADW_EXPANDER_ROW (row), "not listed in SHA256SUMS", r->extra);
      adw_expander_row_set_expanded (ADW_EXPANDER_ROW (row), !ok);
      gtk_list_box_append (GTK_LIST_BOX (v->results), row);
    }
  if (s->results && s->finished_at)
    {
      g_autofree char *when = verify_date (s->finished_at);
      g_autofree char *t = s->results->len == 0
        ? g_strdup_printf ("%s: no archive folders (with a SHA256SUMS) found in %s", when, s->path)
        : g_strdup_printf ("%s%s: %u folder(s) checked, %s", when, s->stopped ? " (stopped)" : "", s->results->len,
                           damaged ? "some are damaged" : "all OK");
      gtk_label_set_text (GTK_LABEL (v->results_title), t);
    }
  else
    gtk_label_set_text (GTK_LABEL (v->results_title), "");
  gtk_widget_set_visible (v->results, s->results && s->results->len);
}

static void
verify_refresh (Verify *v)
{
  BroState *st = bro_app_state ();
  BroVerifyStatus *s = &st->verify;
  g_autofree char *root = bro_state_output_root (st);
  BroCheckRecord *rec = bro_state_check_record (st, root);
  g_autofree char *last = NULL;
  if (rec)
    {
      g_autofree char *when = verify_date (rec->checked_at);
      last = g_strdup_printf ("Output folder %s: last checked %s — %s", root, when, rec->summary);
    }
  else
    last = g_strdup_printf ("Output folder %s: never checked", root);
  if (st->config->archive_check_interval_days > 0)
    {
      char *t = g_strdup_printf ("%s. Checked every %d day(s) (Preferences).", last, st->config->archive_check_interval_days);
      g_free (last);
      last = t;
    }
  gtk_label_set_text (GTK_LABEL (v->last), last);
  gtk_widget_set_visible (v->progress, s->running);
  gtk_widget_set_visible (v->progress_label, s->running);
  gtk_widget_set_visible (v->cancel, s->running);
  gtk_widget_set_sensitive (v->check_root, !s->running);
  gtk_widget_set_sensitive (v->check_folder, !s->running);
  if (s->running)
    {
      g_autofree char *done = bro_format_bytes (s->done), *total = bro_format_bytes (s->total);
      g_autofree char *t = g_strdup_printf ("%s%s%s\n%s of %s", s->folder ? s->folder : s->path, s->file ? " — " : "",
                                            s->file ? s->file : "", done, total);
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (v->progress), s->total > 0 ? (double) s->done / s->total : 0);
      gtk_label_set_text (GTK_LABEL (v->progress_label), t);
    }
  if (s->running != v->shown_running)
    {
      v->shown_running = s->running;
      verify_rebuild_results (v);
    }
}

static void
verify_changed (GtkWidget *root)
{
  verify_refresh (g_object_get_data (G_OBJECT (root), "verify"));
}

static void
on_verify_root (GtkButton *b, Verify *v)
{
  g_autofree char *root = bro_state_output_root (bro_app_state ());
  if (!g_file_test (root, G_FILE_TEST_IS_DIR))
    {
      g_autofree char *msg = g_strdup_printf ("The output folder %s doesn't exist", root);
      bro_state_set_error (bro_app_state (), msg);
      return;
    }
  bro_state_verify_start (bro_app_state (), root, FALSE);
}

static void
verify_folder_chosen (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GFile) f = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (src), res, NULL);
  g_autofree char *path = f ? g_file_get_path (f) : NULL;
  if (path)
    bro_state_verify_start (bro_app_state (), path, FALSE);
}

static void
on_verify_folder (GtkButton *b, Verify *v)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  g_autofree char *root = bro_state_output_root (bro_app_state ());
  g_autoptr (GFile) initial = g_file_new_for_path (root);
  gtk_file_dialog_set_title (d, "Verify an Archive Folder");
  if (g_file_test (root, G_FILE_TEST_IS_DIR))
    gtk_file_dialog_set_initial_folder (d, initial);
  gtk_file_dialog_select_folder (d, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (b))), NULL, verify_folder_chosen, v);
}

static void
on_verify_cancel (GtkButton *b, Verify *v)
{
  bro_state_verify_cancel (bro_app_state ());
}

GtkWidget *
bro_verify_page_new (void)
{
  Verify *v = g_new0 (Verify, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *title = gtk_label_new ("Archive Check");
  GtkWidget *scroll = gtk_scrolled_window_new ();
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);

  gtk_widget_add_css_class (title, "title-2");
  gtk_widget_set_hexpand (title, TRUE);
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  v->check_root = gtk_button_new_with_label ("Check Output Folder");
  gtk_widget_add_css_class (v->check_root, "suggested-action");
  v->check_folder = gtk_button_new_with_label ("Check a Folder…");
  v->cancel = gtk_button_new_with_label ("Stop");
  g_signal_connect (v->check_root, "clicked", G_CALLBACK (on_verify_root), v);
  g_signal_connect (v->check_folder, "clicked", G_CALLBACK (on_verify_folder), v);
  g_signal_connect (v->cancel, "clicked", G_CALLBACK (on_verify_cancel), v);
  gtk_box_append (GTK_BOX (bar), title);
  gtk_box_append (GTK_BOX (bar), v->cancel);
  gtk_box_append (GTK_BOX (bar), v->check_folder);
  gtk_box_append (GTK_BOX (bar), v->check_root);
  gtk_box_append (GTK_BOX (box), bar);
  gtk_box_append (GTK_BOX (box),
                  bro_caption ("Reads every file listed in each archive folder's SHA256SUMS again and compares it with its "
                               "checksum, to find files damaged on the disk (bit rot) or by a bad copy. Files missing from the "
                               "folder and files SHA256SUMS doesn't list are reported too."));
  v->last = bro_caption ("");
  gtk_box_append (GTK_BOX (box), v->last);
  v->progress = gtk_progress_bar_new ();
  v->progress_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (v->progress_label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (v->progress_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class (v->progress_label, "caption");
  gtk_box_append (GTK_BOX (box), v->progress);
  gtk_box_append (GTK_BOX (box), v->progress_label);

  v->results_title = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (v->results_title), 0);
  gtk_label_set_wrap (GTK_LABEL (v->results_title), TRUE);
  gtk_widget_add_css_class (v->results_title, "heading");
  v->results = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (v->results), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (v->results, "boxed-list");
  gtk_widget_set_valign (v->results, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (inner), v->results_title);
  gtk_box_append (GTK_BOX (inner), v->results);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), inner);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (box), scroll);

  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  v->root = box;
  g_object_set_data_full (G_OBJECT (box), "verify", v, g_free);
  g_signal_connect_object (bro_app_state (), "verify-changed", G_CALLBACK (verify_changed), box, G_CONNECT_SWAPPED);
  v->shown_running = !bro_app_state ()->verify.running; /* rebuild on the first refresh */
  verify_refresh (v);
  return box;
}
