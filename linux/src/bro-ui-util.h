/* bro-ui-util.h — small GTK / libadwaita helpers shared by the pages. */
#pragma once

#include <adwaita.h>
#include "bro-state.h"

G_BEGIN_DECLS

/* The application-wide state (set once in main). */
BroState *bro_app_state (void);
void      bro_app_set_state (BroState *state);

/* GClosureNotify that frees the closure data with g_free(). */
void       bro_closure_free (gpointer data, GClosure *closure);

GtkWidget *bro_tag (const char *text, const char *css_class);
GtkWidget *bro_caption (const char *text);
GtkWidget *bro_icon_button (const char *icon, const char *tooltip);
void       bro_open_path (GtkWidget *any, const char *path);
void       bro_copy_text (GtkWidget *any, const char *text);
char      *bro_format_elapsed (gint64 seconds);

/* Log view: a monospace text view that follows a log (BroLogEntry array + "log-added" signal). */
GtkWidget *bro_log_view_new (GObject *source, GPtrArray *entries);

/* Rows bound to config fields. `changed` is called after every edit. */
typedef void (*BroChangedFunc) (gpointer data);

GtkWidget *bro_entry_row (const char *title, char **field, const char *placeholder, BroChangedFunc changed, gpointer data);
GtkWidget *bro_switch_row (const char *title, const char *subtitle, gboolean *field, BroChangedFunc changed, gpointer data);
GtkWidget *bro_spin_row (const char *title, const char *subtitle, int *field, int min, int max, BroChangedFunc changed, gpointer data);
GtkWidget *bro_optional_spin_row (const char *title, const char *subtitle, int *field, int max, BroChangedFunc changed, gpointer data);
GtkWidget *bro_combo_row (const char *title, const char *const *labels, int *field, BroChangedFunc changed, gpointer data);
GtkWidget *bro_path_row (const char *title, char **field, gboolean folder, BroChangedFunc changed, gpointer data);
GtkWidget *bro_duration_row (const char *title, int *field, BroChangedFunc changed, gpointer data);

void bro_pick_path (GtkWidget *parent, gboolean folder, const char *title, void (*done) (const char *path, gpointer data), gpointer data);
void bro_save_path (GtkWidget *parent, const char *title, const char *suggested, void (*done) (const char *path, gpointer data), gpointer data);

G_END_DECLS
