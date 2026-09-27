/* bro-ui-util.c — shared GTK helpers. */
#include "bro-ui-util.h"
#include "bro-robot.h"

#include <stdlib.h>

static BroState *app_state;

BroState *bro_app_state (void) { return app_state; }
void bro_app_set_state (BroState *state) { app_state = state; }

void
bro_closure_free (gpointer data, GClosure *closure)
{
  g_free (data);
}

GtkWidget *
bro_tag (const char *text, const char *css_class)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "tag");
  if (css_class)
    gtk_widget_add_css_class (l, css_class);
  gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
  return l;
}

GtkWidget *
bro_caption (const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "caption");
  gtk_widget_add_css_class (l, "dim-label");
  gtk_label_set_selectable (GTK_LABEL (l), TRUE);
  return l;
}

GtkWidget *
bro_icon_button (const char *icon, const char *tooltip)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (b, tooltip);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (b, "flat");
  return b;
}

void
bro_open_path (GtkWidget *any, const char *path)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  GtkRoot *root = any ? gtk_widget_get_root (any) : NULL;
  if (g_file_query_file_type (f, 0, NULL) == G_FILE_TYPE_DIRECTORY)
    {
      g_autoptr (GtkFileLauncher) l = gtk_file_launcher_new (f);
      gtk_file_launcher_launch (l, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
    }
  else
    {
      g_autoptr (GtkFileLauncher) l = gtk_file_launcher_new (f);
      if (g_str_has_suffix (path, ".txt") || g_str_has_suffix (path, ".json"))
        gtk_file_launcher_launch (l, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
      else
        gtk_file_launcher_open_containing_folder (l, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
    }
}

void
bro_copy_text (GtkWidget *any, const char *text)
{
  gdk_clipboard_set_text (gtk_widget_get_clipboard (any), text);
}

char *
bro_format_elapsed (gint64 s)
{
  if (s >= 3600)
    return g_strdup_printf ("%d:%02d:%02d", (int) (s / 3600), (int) ((s / 60) % 60), (int) (s % 60));
  return g_strdup_printf ("%d:%02d", (int) (s / 60), (int) (s % 60));
}

/* ---- log view ---- */

static void
log_append (GtkTextView *view, BroLogEntry *e)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (view);
  GtkTextIter end;
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (e->time);
  g_autofree char *stamp = g_date_time_format (dt, "%H:%M:%S");
  g_autofree char *line = g_strdup_printf ("%s  %s\n", stamp, e->text);
  const char *tag = e->severity == BRO_SEV_ERROR ? "error" : e->severity == BRO_SEV_WARNING ? "warning"
                    : e->severity == BRO_SEV_DEBUG ? "debug" : NULL;
  gtk_text_buffer_get_end_iter (buf, &end);
  if (tag)
    gtk_text_buffer_insert_with_tags_by_name (buf, &end, line, -1, tag, NULL);
  else
    gtk_text_buffer_insert (buf, &end, line, -1);
  if (gtk_text_buffer_get_line_count (buf) > 6000)
    {
      GtkTextIter a, b;
      gtk_text_buffer_get_start_iter (buf, &a);
      gtk_text_buffer_get_iter_at_line (buf, &b, 1000);
      gtk_text_buffer_delete (buf, &a, &b);
    }
}

static void
on_log_added (GObject *src, gpointer entry, GtkTextView *view)
{
  GtkWidget *scroller = gtk_widget_get_parent (GTK_WIDGET (view));
  GtkAdjustment *adj = GTK_IS_SCROLLED_WINDOW (scroller) ? gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller)) : NULL;
  gboolean at_bottom = !adj || gtk_adjustment_get_value (adj) >= gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj) - 20;
  log_append (view, entry);
  if (at_bottom)
    {
      GtkTextIter end;
      GtkTextBuffer *buf = gtk_text_view_get_buffer (view);
      gtk_text_buffer_get_end_iter (buf, &end);
      GtkTextMark *mark = gtk_text_buffer_create_mark (buf, NULL, &end, FALSE);
      gtk_text_view_scroll_mark_onscreen (view, mark);
      gtk_text_buffer_delete_mark (buf, mark);
    }
}

GtkWidget *
bro_log_view_new (GObject *source, GPtrArray *entries)
{
  GtkWidget *view = gtk_text_view_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 8);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 6);
  gtk_widget_add_css_class (view, "log-view");
  gtk_text_buffer_create_tag (buf, "error", "foreground", "#e01b24", NULL);
  gtk_text_buffer_create_tag (buf, "warning", "foreground", "#c64600", NULL);
  gtk_text_buffer_create_tag (buf, "debug", "foreground", "#77767b", NULL);
  for (guint i = 0; entries && i < entries->len; i++)
    log_append (GTK_TEXT_VIEW (view), entries->pdata[i]);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_add_css_class (scroller, "card");
  if (source)
    g_signal_connect_object (source, "log-added", G_CALLBACK (on_log_added), view, 0);
  return scroller;
}

/* ---- bound rows ---- */

typedef struct {
  gpointer field;
  BroChangedFunc changed;
  gpointer data;
  int extra;
} Binding;

static Binding *
binding_new (gpointer field, BroChangedFunc changed, gpointer data)
{
  Binding *b = g_new0 (Binding, 1);
  b->field = field;
  b->changed = changed;
  b->data = data;
  return b;
}

static void
notify_changed (Binding *b)
{
  if (b->changed)
    b->changed (b->data);
}

static void
on_entry_changed (GtkEditable *e, Binding *b)
{
  char **field = b->field;
  const char *text = gtk_editable_get_text (e);
  if (g_strcmp0 (*field, text) == 0)
    return;
  g_free (*field);
  *field = g_strdup (text);
  notify_changed (b);
}

GtkWidget *
bro_entry_row (const char *title, char **field, const char *placeholder, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), *field ? *field : "");
  if (placeholder && *placeholder)
    gtk_widget_set_tooltip_text (row, placeholder);
  g_signal_connect_data (row, "changed", G_CALLBACK (on_entry_changed), binding_new (field, changed, data), bro_closure_free, 0);
  return row;
}

static void
on_switch (GObject *row, GParamSpec *pspec, Binding *b)
{
  *(gboolean *) b->field = adw_switch_row_get_active (ADW_SWITCH_ROW (row));
  notify_changed (b);
}

GtkWidget *
bro_switch_row (const char *title, const char *subtitle, gboolean *field, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_switch_row_set_active (ADW_SWITCH_ROW (row), *field);
  g_signal_connect_data (row, "notify::active", G_CALLBACK (on_switch), binding_new (field, changed, data), bro_closure_free, 0);
  return row;
}

static void
on_spin (GObject *row, GParamSpec *pspec, Binding *b)
{
  int v = (int) adw_spin_row_get_value (ADW_SPIN_ROW (row));
  *(int *) b->field = (b->extra && v == 0) ? -1 : v;
  notify_changed (b);
}

GtkWidget *
bro_spin_row (const char *title, const char *subtitle, int *field, int min, int max, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = adw_spin_row_new_with_range (min, max, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_spin_row_set_value (ADW_SPIN_ROW (row), *field);
  g_signal_connect_data (row, "notify::value", G_CALLBACK (on_spin), binding_new (field, changed, data), bro_closure_free, 0);
  return row;
}

GtkWidget *
bro_optional_spin_row (const char *title, const char *subtitle, int *field, int max, BroChangedFunc changed, gpointer data)
{
  /* 0 shown as "default" (stored as -1). */
  GtkWidget *row = adw_spin_row_new_with_range (0, max, 1);
  Binding *b = binding_new (field, changed, data);
  b->extra = 1;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle ? subtitle : "0 = MakeMKV default");
  adw_spin_row_set_value (ADW_SPIN_ROW (row), MAX (*field, 0));
  g_signal_connect_data (row, "notify::value", G_CALLBACK (on_spin), b, bro_closure_free, 0);
  return row;
}

static void
on_combo (GObject *row, GParamSpec *pspec, Binding *b)
{
  *(int *) b->field = (int) adw_combo_row_get_selected (ADW_COMBO_ROW (row));
  notify_changed (b);
}

GtkWidget *
bro_combo_row (const char *title, const char *const *labels, int *field, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = adw_combo_row_new ();
  g_autoptr (GtkStringList) model = gtk_string_list_new (labels);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (model));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), (guint) MAX (*field, 0));
  g_signal_connect_data (row, "notify::selected", G_CALLBACK (on_combo), binding_new (field, changed, data), bro_closure_free, 0);
  return row;
}

typedef struct {
  GtkWidget *row;
} PathPick;

static void
path_picked (const char *path, gpointer data)
{
  gtk_editable_set_text (GTK_EDITABLE (data), path);
}

static void
on_browse (GtkButton *btn, GtkWidget *row)
{
  gboolean folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "folder"));
  bro_pick_path (row, folder, adw_preferences_row_get_title (ADW_PREFERENCES_ROW (row)), path_picked, row);
}

GtkWidget *
bro_path_row (const char *title, char **field, gboolean folder, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = bro_entry_row (title, field, NULL, changed, data);
  GtkWidget *btn = bro_icon_button (folder ? "folder-open-symbolic" : "document-open-symbolic", "Choose…");
  g_object_set_data (G_OBJECT (row), "folder", GINT_TO_POINTER (folder));
  g_signal_connect (btn, "clicked", G_CALLBACK (on_browse), row);
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (row), btn);
  return row;
}

static void
on_duration (GtkEditable *e, Binding *b)
{
  *(int *) b->field = bro_parse_duration (gtk_editable_get_text (e));
  notify_changed (b);
}

GtkWidget *
bro_duration_row (const char *title, int *field, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = adw_entry_row_new ();
  g_autofree char *text = *field > 0 ? bro_format_duration (*field) : g_strdup ("");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), text);
  gtk_widget_set_tooltip_text (row, "h:mm:ss, m:ss or seconds; empty = no limit");
  g_signal_connect_data (row, "changed", G_CALLBACK (on_duration), binding_new (field, changed, data), bro_closure_free, 0);
  return row;
}

/* ---- file dialogs ---- */

typedef struct {
  void (*done) (const char *path, gpointer data);
  gpointer data;
  gboolean folder;
  gboolean save;
} PickData;

static void
pick_finished (GObject *src, GAsyncResult *res, gpointer user)
{
  PickData *p = user;
  g_autoptr (GFile) f = NULL;
  if (p->save)
    f = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (src), res, NULL);
  else if (p->folder)
    f = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (src), res, NULL);
  else
    f = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (f)
    {
      g_autofree char *path = g_file_get_path (f);
      if (path)
        p->done (path, p->data);
    }
  g_free (p);
}

void
bro_pick_path (GtkWidget *parent, gboolean folder, const char *title, void (*done) (const char *path, gpointer data), gpointer data)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  PickData *p = g_new0 (PickData, 1);
  GtkRoot *root = parent ? gtk_widget_get_root (parent) : NULL;
  p->done = done;
  p->data = data;
  p->folder = folder;
  gtk_file_dialog_set_title (d, title ? title : "Choose");
  if (folder)
    gtk_file_dialog_select_folder (d, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, pick_finished, p);
  else
    gtk_file_dialog_open (d, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, pick_finished, p);
}

void
bro_save_path (GtkWidget *parent, const char *title, const char *suggested, void (*done) (const char *path, gpointer data), gpointer data)
{
  g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
  PickData *p = g_new0 (PickData, 1);
  GtkRoot *root = parent ? gtk_widget_get_root (parent) : NULL;
  p->done = done;
  p->data = data;
  p->save = TRUE;
  gtk_file_dialog_set_title (d, title);
  gtk_file_dialog_set_initial_name (d, suggested);
  gtk_file_dialog_save (d, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, pick_finished, p);
}
