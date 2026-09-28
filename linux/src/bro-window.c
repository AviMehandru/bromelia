/* bro-window.c — main window: sidebar of drives / images / jobs and the content area. */
#include "bro-pages.h"

struct _BroWindow {
  AdwApplicationWindow parent_instance;
  AdwNavigationSplitView *split;
  AdwToastOverlay *toasts;
  AdwBanner *banner;
  GtkListBox *sidebar;
  AdwToolbarView *content_view;
  AdwNavigationPage *content_page;
  GtkLabel *status_label;
  char *current_tag;
  gboolean rebuilding;
  guint inhibit_cookie;
};

G_DEFINE_FINAL_TYPE (BroWindow, bro_window, ADW_TYPE_APPLICATION_WINDOW)

static void rebuild_sidebar (BroWindow *self);

/* Keeps the computer from suspending (and the session from going idle) while jobs run. */
static void
update_inhibit (BroWindow *self)
{
  BroState *st = bro_app_state ();
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  gboolean busy = bro_state_active_count (st) > 0 && st->config->prevent_sleep;
  if (!app)
    return;
  if (busy && !self->inhibit_cookie)
    self->inhibit_cookie = gtk_application_inhibit (app, GTK_WINDOW (self), GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_IDLE,
                                                    "Ripping discs");
  else if (!busy && self->inhibit_cookie)
    {
      gtk_application_uninhibit (app, self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }
}

static GtkWidget *
sidebar_row (const char *tag, const char *icon, const char *title, const char *subtitle, gboolean dim)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *img = gtk_image_new_from_icon_name (icon);
  GtkWidget *t = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (t), 0);
  gtk_label_set_ellipsize (GTK_LABEL (t), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (labels), t);
  if (subtitle && *subtitle)
    {
      GtkWidget *s = gtk_label_new (subtitle);
      gtk_label_set_xalign (GTK_LABEL (s), 0);
      gtk_label_set_ellipsize (GTK_LABEL (s), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class (s, "sidebar-subtitle");
      gtk_box_append (GTK_BOX (labels), s);
    }
  gtk_box_append (GTK_BOX (box), img);
  gtk_box_append (GTK_BOX (box), labels);
  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data_full (G_OBJECT (row), "tag", g_strdup (tag), g_free);
  if (dim)
    gtk_widget_set_opacity (row, 0.6);
  return row;
}

static GtkWidget *
sidebar_header (const char *title)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *l = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "heading");
  gtk_widget_add_css_class (l, "dim-label");
  gtk_widget_set_margin_top (l, 12);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  return row;
}

static char *
drive_subtitle (BroState *st, BroDriveItem *it)
{
  BroJob *job = bro_state_active_job (st, it->lane);
  if (job && job->state == BRO_JOB_RUNNING)
    return g_strdup_printf ("%s · %d%%", job->phase, (int) (bro_job_overall (job) * 100));
  if (job && job->state == BRO_JOB_WAITING)
    return g_strdup ("Automatic rip starting");
  if (job)
    return g_strdup ("Queued");
  if (!it->entry)
    return g_strdup ("Disconnected");
  if (it->entry->state == BRO_DRIVE_INSERTED)
    return *it->entry->disc_name ? g_strdup_printf ("%s · %s", it->entry->disc_name, bro_disc_type_name (it->entry->flags))
                                 : g_strdup (bro_disc_type_name (it->entry->flags));
  return g_strdup (bro_drive_state_name (it->entry->state));
}

static void
rebuild_sidebar (BroWindow *self)
{
  BroState *st = bro_app_state ();
  g_autoptr (GPtrArray) items = bro_state_drive_items (st);
  GtkListBoxRow *select = NULL;
  GtkWidget *child;
  int active = 0;

  self->rebuilding = TRUE;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->sidebar))))
    gtk_list_box_remove (self->sidebar, child);

  gtk_list_box_append (self->sidebar, sidebar_header ("Drives"));
  for (guint i = 0; i < items->len; i++)
    {
      BroDriveItem *it = items->pdata[i];
      g_autofree char *tag = g_strdup_printf ("drive:%s", it->id);
      g_autofree char *name = bro_drive_item_name (it);
      g_autofree char *title = g_strdup_printf ("%s%s%s", name, it->config == NULL && it->entry ? " (not set up)" : "",
                                                it->config && it->config->automation.auto_rip_on_insert ? " ⚡" : "");
      g_autofree char *sub = drive_subtitle (st, it);
      GtkWidget *row = sidebar_row (tag, it->entry && it->entry->state == BRO_DRIVE_INSERTED ? "media-optical-symbolic" : "drive-optical-symbolic",
                                    title, sub, it->entry == NULL);
      gtk_list_box_append (self->sidebar, row);
      if (g_strcmp0 (tag, self->current_tag) == 0)
        select = GTK_LIST_BOX_ROW (row);
    }
  if (items->len == 0)
    {
      GtkWidget *row = sidebar_row ("", "drive-optical-symbolic", st->scanning ? "Scanning…" : "No optical drives found", NULL, TRUE);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
      gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
      gtk_list_box_append (self->sidebar, row);
    }

  if (st->file_sessions->len)
    {
      gtk_list_box_append (self->sidebar, sidebar_header ("Images & Folders"));
      for (guint i = 0; i < st->file_sessions->len; i++)
        {
          BroSession *s = st->file_sessions->pdata[i];
          g_autofree char *tag = g_strdup_printf ("source:%s", s->id);
          g_autofree char *name = bro_source_display_name (s->source);
          const char *sub = s->loading ? "Reading…" : s->info ? bro_disc_info_name (s->info) : (s->error ? s->error : "");
          GtkWidget *row = sidebar_row (tag, s->source->kind == BRO_SOURCE_ISO ? "media-optical-symbolic" : "folder-symbolic", name, sub, FALSE);
          gtk_list_box_append (self->sidebar, row);
          if (g_strcmp0 (tag, self->current_tag) == 0)
            select = GTK_LIST_BOX_ROW (row);
        }
    }

  gtk_list_box_append (self->sidebar, sidebar_header ("Jobs"));
  for (guint i = 0; i < st->jobs->len; i++)
    active += !bro_job_state_finished (((BroJob *) st->jobs->pdata[i])->state);
  {
    g_autofree char *q = active ? g_strdup_printf ("Queue (%d)", active) : g_strdup ("Queue");
    GtkWidget *row = sidebar_row ("queue", "view-list-symbolic", q, NULL, FALSE);
    gtk_list_box_append (self->sidebar, row);
    if (g_strcmp0 ("queue", self->current_tag) == 0)
      select = GTK_LIST_BOX_ROW (row);
    row = sidebar_row ("history", "document-open-recent-symbolic", "History", NULL, FALSE);
    gtk_list_box_append (self->sidebar, row);
    if (g_strcmp0 ("history", self->current_tag) == 0)
      select = GTK_LIST_BOX_ROW (row);
  }
  if (select)
    gtk_list_box_select_row (self->sidebar, select);
  self->rebuilding = FALSE;
}

static void
set_content (BroWindow *self, const char *title, GtkWidget *content)
{
  adw_navigation_page_set_title (self->content_page, title);
  adw_toolbar_view_set_content (self->content_view, content);
}

void
bro_window_navigate (BroWindow *self, const char *tag)
{
  g_free (self->current_tag);
  self->current_tag = g_strdup (tag);
  if (g_str_has_prefix (tag, "drive:") || g_str_has_prefix (tag, "source:"))
    set_content (self, "Drive", bro_drive_page_new (tag));
  else if (g_str_equal (tag, "history"))
    set_content (self, "History", bro_history_page_new ());
  else if (g_str_equal (tag, "tools"))
    set_content (self, "Drive Tools", bro_tools_page_new ());
  else
    set_content (self, "Queue", bro_queue_page_new ());
  rebuild_sidebar (self);
  adw_navigation_split_view_set_show_content (self->split, TRUE);
}

static void
on_row_selected (GtkListBox *box, GtkListBoxRow *row, BroWindow *self)
{
  const char *tag;
  if (self->rebuilding || !row)
    return;
  tag = g_object_get_data (G_OBJECT (row), "tag");
  if (tag && *tag && g_strcmp0 (tag, self->current_tag) != 0)
    bro_window_navigate (self, tag);
}

void
bro_window_toast (BroWindow *self, const char *text)
{
  adw_toast_overlay_add_toast (self->toasts, adw_toast_new (text));
}

static void
on_status_changed (BroState *st, BroWindow *self)
{
  if (st->last_error && *st->last_error)
    {
      adw_banner_set_title (self->banner, st->last_error);
      adw_banner_set_revealed (self->banner, TRUE);
    }
  else
    adw_banner_set_revealed (self->banner, FALSE);
  {
    g_autofree char *when = NULL;
    if (st->last_scan)
      {
        g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (st->last_scan);
        when = g_date_time_format (dt, "%X");
      }
    g_autofree char *text = st->scanning ? g_strdup ("Scanning drives…")
                            : g_strdup_printf ("%s%s%s", st->makemkv_version, when ? "\nScanned " : "", when ? when : "");
    gtk_label_set_text (self->status_label, text);
  }
  rebuild_sidebar (self);
}

static void
on_banner_button (AdwBanner *b, BroWindow *self)
{
  bro_state_set_error (bro_app_state (), NULL);
}

/* ---- opening images ---- */

static void
open_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroWindow *self = data;
  g_autoptr (GListModel) files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (src), res, NULL);
  for (guint i = 0; files && i < g_list_model_get_n_items (files); i++)
    {
      g_autoptr (GFile) f = g_list_model_get_item (files, i);
      g_autofree char *path = g_file_get_path (f);
      if (path)
        {
          BroSession *s = bro_state_open_file (bro_app_state (), path);
          g_autofree char *tag = g_strdup_printf ("source:%s", s->id);
          bro_window_navigate (self, tag);
        }
    }
}

static void
folder_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroWindow *self = data;
  g_autoptr (GFile) f = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (src), res, NULL);
  g_autofree char *path = f ? g_file_get_path (f) : NULL;
  if (path)
    {
      BroSession *s = bro_state_open_file (bro_app_state (), path);
      g_autofree char *tag = g_strdup_printf ("source:%s", s->id);
      bro_window_navigate (self, tag);
    }
}

void
bro_window_open_sources (BroWindow *self)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) iso = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  gtk_file_filter_set_name (iso, "Disc images");
  gtk_file_filter_add_pattern (iso, "*.iso");
  gtk_file_filter_add_pattern (iso, "*.ISO");
  gtk_file_filter_add_mime_type (iso, "application/x-cd-image");
  g_list_store_append (filters, iso);
  gtk_file_dialog_set_filters (d, G_LIST_MODEL (filters));
  gtk_file_dialog_set_title (d, "Open Disc Image");
  gtk_file_dialog_open_multiple (d, GTK_WINDOW (self), NULL, open_done, self);
}

static void
action_open (GSimpleAction *a, GVariant *p, gpointer self) { bro_window_open_sources (self); }

static void
action_open_folder (GSimpleAction *a, GVariant *p, gpointer self)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (d, "Open BDMV / VIDEO_TS Folder");
  gtk_file_dialog_select_folder (d, GTK_WINDOW (self), NULL, folder_done, self);
}

static void
action_rescan (GSimpleAction *a, GVariant *p, gpointer self) { bro_state_refresh_drives (bro_app_state (), TRUE); }
static void
action_queue (GSimpleAction *a, GVariant *p, gpointer self) { bro_window_navigate (self, "queue"); }
static void
action_history (GSimpleAction *a, GVariant *p, gpointer self) { bro_window_navigate (self, "history"); }
static void
action_tools (GSimpleAction *a, GVariant *p, gpointer self) { bro_window_navigate (self, "tools"); }
static void
action_prefs (GSimpleAction *a, GVariant *p, gpointer self) { bro_preferences_present (GTK_WIDGET (self)); }

static void
action_about (GSimpleAction *a, GVariant *p, gpointer self)
{
  AdwDialog *about = adw_about_dialog_new ();
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (about), "Bromelia");
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (about), APP_ID);
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (about), PACKAGE_VERSION);
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (about),
                                 "A multi-drive front-end for makemkvcon. Each drive has its own MakeMKV settings, profile, "
                                 "title rules and post-processing.\n\nMakeMKV is a product of GuinpinSoft inc. and is not affiliated with Bromelia.");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (about), GTK_LICENSE_GPL_3_0);
  adw_dialog_present (about, GTK_WIDGET (self));
}

static gboolean
on_close_request (GtkWindow *win, gpointer data)
{
  BroState *st = bro_app_state ();
  bro_state_save_now (st);
  return FALSE;
}

static gboolean
on_drop (GtkDropTarget *target, const GValue *value, double x, double y, BroWindow *self)
{
  GSList *files;
  if (!G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    return FALSE;
  files = gdk_file_list_get_files (g_value_get_boxed (value));
  for (GSList *l = files; l; l = l->next)
    {
      g_autofree char *path = g_file_get_path (l->data);
      if (path)
        {
          BroSession *s = bro_state_open_file (bro_app_state (), path);
          g_autofree char *tag = g_strdup_printf ("source:%s", s->id);
          bro_window_navigate (self, tag);
        }
    }
  g_slist_free (files);
  return TRUE;
}

static void
bro_window_finalize (GObject *obj)
{
  BroWindow *self = BRO_WINDOW (obj);
  g_free (self->current_tag);
  G_OBJECT_CLASS (bro_window_parent_class)->finalize (obj);
}

static void
bro_window_class_init (BroWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_window_finalize;
}

static void
bro_window_init (BroWindow *self)
{
  static const GActionEntry actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "open-folder", action_open_folder, NULL, NULL, NULL, { 0 } },
    { "rescan", action_rescan, NULL, NULL, NULL, { 0 } },
    { "queue", action_queue, NULL, NULL, NULL, { 0 } },
    { "history", action_history, NULL, NULL, NULL, { 0 } },
    { "tools", action_tools, NULL, NULL, NULL, { 0 } },
    { "preferences", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
  };
  GtkWidget *sidebar_view, *sidebar_header, *sidebar_scroll, *sidebar_box, *menu_button, *open_button, *rescan_button;
  AdwNavigationPage *sidebar_page;
  GMenu *menu = g_menu_new ();
  GtkDropTarget *drop;

  g_action_map_add_action_entries (G_ACTION_MAP (self), actions, G_N_ELEMENTS (actions), self);
  gtk_window_set_title (GTK_WINDOW (self), "Bromelia");
  gtk_window_set_default_size (GTK_WINDOW (self), 1200, 800);

  /* Sidebar. */
  self->sidebar = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->sidebar), "navigation-sidebar");
  g_signal_connect (self->sidebar, "row-selected", G_CALLBACK (on_row_selected), self);
  sidebar_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sidebar_scroll), GTK_WIDGET (self->sidebar));
  gtk_widget_set_vexpand (sidebar_scroll, TRUE);
  self->status_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (self->status_label, 0);
  gtk_label_set_wrap (self->status_label, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->status_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->status_label), "dim-label");
  gtk_widget_set_margin_start (GTK_WIDGET (self->status_label), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->status_label), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->status_label), 8);
  sidebar_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (sidebar_box), sidebar_scroll);
  gtk_box_append (GTK_BOX (sidebar_box), GTK_WIDGET (self->status_label));

  g_menu_append (menu, "Open Disc Image…", "win.open");
  g_menu_append (menu, "Open BDMV / VIDEO_TS Folder…", "win.open-folder");
  g_menu_append (menu, "Rescan Drives", "win.rescan");
  g_menu_append (menu, "Drive Tools", "win.tools");
  g_menu_append (menu, "Preferences", "win.preferences");
  g_menu_append (menu, "About Bromelia", "win.about");
  menu_button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button), G_MENU_MODEL (menu));
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_button), TRUE);
  g_object_unref (menu);
  open_button = bro_icon_button ("document-open-symbolic", "Open a disc image");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (open_button), "win.open");
  rescan_button = bro_icon_button ("view-refresh-symbolic", "Rescan drives");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (rescan_button), "win.rescan");

  sidebar_header = adw_header_bar_new ();
  adw_header_bar_set_show_title (ADW_HEADER_BAR (sidebar_header), FALSE);
  adw_header_bar_pack_start (ADW_HEADER_BAR (sidebar_header), open_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (sidebar_header), menu_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (sidebar_header), rescan_button);
  sidebar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (sidebar_view), sidebar_header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (sidebar_view), sidebar_box);
  sidebar_page = adw_navigation_page_new (sidebar_view, "Bromelia");

  /* Content. */
  self->content_view = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
  adw_toolbar_view_add_top_bar (self->content_view, adw_header_bar_new ());
  self->banner = ADW_BANNER (adw_banner_new (""));
  adw_banner_set_button_label (self->banner, "Dismiss");
  g_signal_connect (self->banner, "button-clicked", G_CALLBACK (on_banner_button), self);
  adw_toolbar_view_add_top_bar (self->content_view, GTK_WIDGET (self->banner));
  self->content_page = adw_navigation_page_new (GTK_WIDGET (self->content_view), "Queue");

  self->split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  adw_navigation_split_view_set_sidebar (self->split, sidebar_page);
  adw_navigation_split_view_set_content (self->split, self->content_page);
  adw_navigation_split_view_set_min_sidebar_width (self->split, 240);
  adw_navigation_split_view_set_max_sidebar_width (self->split, 340);

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, GTK_WIDGET (self->split));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), GTK_WIDGET (self->toasts));

  /* Breakpoint: collapse the sidebar on narrow windows. */
  {
    AdwBreakpoint *bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 720sp"));
    GValue v = G_VALUE_INIT;
    g_value_init (&v, G_TYPE_BOOLEAN);
    g_value_set_boolean (&v, TRUE);
    adw_breakpoint_add_setter (bp, G_OBJECT (self->split), "collapsed", &v);
    adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (self), bp);
    g_value_unset (&v);
  }

  drop = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (drop, "drop", G_CALLBACK (on_drop), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drop));
  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), NULL);

  g_signal_connect_object (bro_app_state (), "drives-changed", G_CALLBACK (rebuild_sidebar), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (bro_app_state (), "jobs-changed", G_CALLBACK (rebuild_sidebar), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (bro_app_state (), "tick", G_CALLBACK (rebuild_sidebar), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (bro_app_state (), "jobs-changed", G_CALLBACK (update_inhibit), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (bro_app_state (), "tick", G_CALLBACK (update_inhibit), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (bro_app_state (), "status-changed", G_CALLBACK (on_status_changed), self, 0);
  bro_window_navigate (self, "queue");
  on_status_changed (bro_app_state (), self);
}

BroWindow *
bro_window_new (AdwApplication *app)
{
  return g_object_new (BRO_TYPE_WINDOW, "application", app, NULL);
}
