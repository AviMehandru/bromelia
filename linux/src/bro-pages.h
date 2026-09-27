/* bro-pages.h — top-level widgets of the Bromelia window. */
#pragma once

#include <adwaita.h>
#include "bro-state.h"
#include "bro-ui-util.h"

G_BEGIN_DECLS

GtkWidget *bro_job_card_new (BroJob *job);
GtkWidget *bro_drive_page_new (const char *tag); /* "drive:<id>" or "source:<key>" */
GtkWidget *bro_queue_page_new (void);
GtkWidget *bro_history_page_new (void);
GtkWidget *bro_tools_page_new (void);

void       bro_config_dialog_present (GtkWidget *parent, const char *config_id);
void       bro_preferences_present (GtkWidget *parent);
GtkWidget *bro_catalog_page_new (GHashTable *settings, GHashTable *global, gboolean drive_mode, BroChangedFunc changed, gpointer data);
GtkWidget *bro_drive_config_pages_add (AdwPreferencesDialog *dialog, BroDriveConfig *config, gboolean is_default,
                                       BroChangedFunc changed, gpointer data);

#define BRO_TYPE_WINDOW (bro_window_get_type ())
G_DECLARE_FINAL_TYPE (BroWindow, bro_window, BRO, WINDOW, AdwApplicationWindow)

BroWindow *bro_window_new (AdwApplication *app);
void       bro_window_navigate (BroWindow *self, const char *tag);
void       bro_window_open_sources (BroWindow *self);
void       bro_window_toast (BroWindow *self, const char *text);

G_END_DECLS
