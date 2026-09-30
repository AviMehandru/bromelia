/* bro-preferences.c — application preferences: tools, global MakeMKV settings, default drive, drives & presets, registration. */
#include "bro-pages.h"
#include "bro-integrations.h"

static void
changed_cb (gpointer data)
{
  bro_state_config_changed (bro_app_state ());
}

static AdwPreferencesGroup *
group (AdwPreferencesPage *page, const char *title, const char *description)
{
  GtkWidget *g = adw_preferences_group_new ();
  if (title)
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g), title);
  if (description)
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g), description);
  adw_preferences_page_add (page, ADW_PREFERENCES_GROUP (g));
  return ADW_PREFERENCES_GROUP (g);
}

static GtkWidget *
button_row (const char *title, const char *subtitle, const char *label, GCallback cb, gpointer data)
{
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *b = gtk_button_new_with_label (label);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  g_signal_connect (b, "clicked", cb, data);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), b);
  return row;
}

/* ---- general ---- */

static void
on_import (GtkButton *b, gpointer data)
{
  g_autoptr (GHashTable) s = bro_makemkv_installed_settings ();
  bro_state_import_settings (bro_app_state (), s);
}

static void
on_open_data (GtkButton *b, gpointer data)
{
  g_autofree char *dir = bro_data_dir ();
  g_mkdir_with_parents (dir, 0755);
  bro_open_path (GTK_WIDGET (b), dir);
}

static AdwPreferencesPage *
general_page (void)
{
  BroState *st = bro_app_state ();
  BroAppConfig *c = st->config;
  GtkWidget *page = adw_preferences_page_new ();
  AdwPreferencesGroup *g;
  g_autofree char *mk = bro_state_makemkvcon (st);
  g_autofree char *mm = bro_state_mkvmerge (st);
  g_autofree char *tools = g_strdup_printf ("Leave empty to find the tools automatically.\nmakemkvcon: %s\nmkvmerge: %s",
                                            mk ? mk : "not found — install MakeMKV",
                                            mm ? mm : "not found — install MKVToolNix to choose individual tracks");
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), "General");
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "preferences-other-symbolic");

  g = group (ADW_PREFERENCES_PAGE (page), "Tools", tools);
  adw_preferences_group_add (g, bro_path_row ("makemkvcon", &c->makemkvcon_path, FALSE, changed_cb, NULL));
  adw_preferences_group_add (g, bro_path_row ("mkvmerge", &c->mkvmerge_path, FALSE, changed_cb, NULL));
  g = group (ADW_PREFERENCES_PAGE (page), "Output", NULL);
  adw_preferences_group_add (g, bro_path_row ("Default output folder", &c->output_root, TRUE, changed_cb, NULL));
  g = group (ADW_PREFERENCES_PAGE (page), "Drive detection",
             "Disc insertion and removal are also detected through GIO. Polling runs makemkvcon to refresh drive and disc names; "
             "pausing it during rips avoids disturbing busy drives.");
  adw_preferences_group_add (g, bro_spin_row ("Poll drives every", "seconds (0 = only on media events)", &c->poll_interval_seconds, 0, 3600, changed_cb, NULL));
  adw_preferences_group_add (g, bro_switch_row ("Keep polling while jobs are running", NULL, &c->poll_while_ripping, changed_cb, NULL));
  g = group (ADW_PREFERENCES_PAGE (page), "Jobs", NULL);
  adw_preferences_group_add (g, bro_spin_row ("Maximum simultaneous jobs", "0 = one per drive, no global limit", &c->max_concurrent_jobs, 0, 64, changed_cb, NULL));
  adw_preferences_group_add (g, bro_spin_row ("Keep history for", "jobs", &c->history_limit, 10, 100000, changed_cb, NULL));
  adw_preferences_group_add (g, bro_spin_row ("Stop a stuck rip after", "minutes without output (0 = never)", &c->stall_timeout_minutes, 0, 1440, changed_cb, NULL));
  adw_preferences_group_add (g, bro_switch_row ("Keep the computer awake while jobs run", NULL, &c->prevent_sleep, changed_cb, NULL));
  adw_preferences_group_add (g, bro_spin_row ("Background post-processing", "steps at a time (encoding, uploads)", &c->background_jobs, 1, 16, changed_cb, NULL));
  g = group (ADW_PREFERENCES_PAGE (page), "Archive check",
             "Reads the output folder's archives again and compares every file with SHA256SUMS, to find damaged or missing files. "
             "The result is sent to the notifications. Verify Archive… in the menu checks now.");
  adw_preferences_group_add (g, bro_spin_row ("Check archives every", "days (0 = never)", &c->archive_check_interval_days, 0, 3650, changed_cb, NULL));
  g = group (ADW_PREFERENCES_PAGE (page), "MakeMKV", st->makemkv_version && *st->makemkv_version ? st->makemkv_version : NULL);
  adw_preferences_group_add (g, button_row ("Import settings from MakeMKV", "Copies ~/.MakeMKV/settings.conf into the global settings", "Import", G_CALLBACK (on_import), NULL));
  adw_preferences_group_add (g, button_row ("Bromelia data folder", "Job logs, manifests and history", "Open", G_CALLBACK (on_open_data), NULL));
  for (guint i = 0; i < st->scan_messages->len; i++)
    {
      GtkWidget *row = adw_action_row_new ();
      g_autofree char *m = g_markup_escape_text (st->scan_messages->pdata[i], -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), m);
      adw_preferences_group_add (g, row);
    }
  return ADW_PREFERENCES_PAGE (page);
}

/* ---- drives & presets ---- */

static void drives_rebuild (AdwPreferencesPage *page);

static void
on_edit_drive (GtkButton *b, gpointer data)
{
  bro_config_dialog_present (GTK_WIDGET (b), g_object_get_data (G_OBJECT (b), "id"));
}

static void
on_duplicate_drive (GtkButton *b, AdwPreferencesPage *page)
{
  BroState *st = bro_app_state ();
  BroDriveConfig *src = bro_app_config_drive_by_id (st->config, g_object_get_data (G_OBJECT (b), "id"));
  BroDriveConfig *c;
  if (!src)
    return;
  c = bro_drive_config_copy (src);
  g_free (c->id);
  c->id = g_uuid_string_random ();
  {
    char *n = g_strdup_printf ("%s copy", c->name);
    g_free (c->name);
    c->name = n;
  }
  g_free (c->match_drive_name);
  c->match_drive_name = g_strdup ("");
  g_free (c->match_device);
  c->match_device = g_strdup ("");
  g_ptr_array_add (st->config->drives, c);
  bro_state_config_changed (st);
  drives_rebuild (page);
}

static void
on_delete_preset (GtkButton *b, AdwPreferencesPage *page)
{
  BroState *st = bro_app_state ();
  g_ptr_array_remove (st->config->presets, g_object_get_data (G_OBJECT (b), "preset"));
  bro_state_config_changed (st);
  drives_rebuild (page);
}

static void
export_to (const char *path, gpointer data)
{
  BroState *st = bro_app_state ();
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL;
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "format");
  json_builder_add_string_value (b, "bromelia-drives");
  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, 1);
  json_builder_set_member_name (b, "drives");
  json_builder_begin_array (b);
  for (guint i = 0; i < st->config->drives->len; i++)
    json_builder_add_value (b, bro_drive_config_to_json (st->config->drives->pdata[i]));
  json_builder_end_array (b);
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  /* Presets are exported through the app config serialiser to keep one code path. */
  {
    g_autoptr (JsonNode) app = bro_app_config_to_json (st->config);
    JsonObject *ao = json_node_get_object (app);
    json_object_set_member (json_node_get_object (root), "presets", json_node_copy (json_object_get_member (ao, "presets")));
  }
  text = bro_json_to_string (root, TRUE);
  g_file_set_contents (path, text, -1, NULL);
}

static void
on_export (GtkButton *b, gpointer data)
{
  bro_save_path (GTK_WIDGET (b), "Export drives and presets", "bromelia-drives.json", export_to, NULL);
}

static void
import_from (const char *path, gpointer data)
{
  AdwPreferencesPage *page = data;
  BroState *st = bro_app_state ();
  g_autofree char *text = NULL;
  g_autoptr (BroAppConfig) imported = NULL;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return;
  /* The export bundle has the same "drives" / "presets" members as the app configuration. */
  imported = bro_app_config_parse (text, NULL);
  if (!imported)
    return;
  for (guint i = 0; i < imported->drives->len; i++)
    {
      BroDriveConfig *d = imported->drives->pdata[i];
      BroDriveConfig *existing = bro_app_config_drive_by_id (st->config, d->id);
      if (existing && existing != st->config->default_drive)
        g_ptr_array_remove (st->config->drives, existing);
      g_ptr_array_add (st->config->drives, bro_drive_config_copy (d));
    }
  for (guint i = 0; i < imported->presets->len; i++)
    {
      BroPreset *p = imported->presets->pdata[i];
      gboolean exists = FALSE;
      for (guint k = 0; k < st->config->presets->len; k++)
        exists |= g_strcmp0 (((BroPreset *) st->config->presets->pdata[k])->id, p->id) == 0;
      if (!exists)
        {
          BroPreset *copy = bro_preset_new (p->name, p->config);
          g_free (copy->id);
          copy->id = g_strdup (p->id);
          g_ptr_array_add (st->config->presets, copy);
        }
    }
  bro_state_config_changed (st);
  drives_rebuild (page);
}

static void
on_import_drives (GtkButton *b, AdwPreferencesPage *page)
{
  bro_pick_path (GTK_WIDGET (b), FALSE, "Import drives and presets", import_from, page);
}

static void
on_edit_default (GtkButton *b, gpointer data)
{
  bro_config_dialog_present (GTK_WIDGET (b), bro_app_state ()->config->default_drive->id);
}

static void
preset_renamed (GtkEditable *e, gpointer data)
{
  BroPreset *p = g_object_get_data (G_OBJECT (e), "preset");
  g_free (p->name);
  p->name = g_strdup (gtk_editable_get_text (e));
  bro_state_config_changed (bro_app_state ());
}

static void
drives_rebuild (AdwPreferencesPage *page)
{
  BroState *st = bro_app_state ();
  GPtrArray *groups = g_object_get_data (G_OBJECT (page), "groups");
  AdwPreferencesGroup *g;
  if (groups)
    for (guint i = 0; i < groups->len; i++)
      adw_preferences_page_remove (page, groups->pdata[i]);
  groups = g_ptr_array_new ();
  g_object_set_data_full (G_OBJECT (page), "groups", groups, (GDestroyNotify) g_ptr_array_unref);

  g = group (page, "Default configuration", "Used for drives that are not set up and for disc images; new drive configurations start as a copy.");
  g_ptr_array_add (groups, g);
  adw_preferences_group_add (g, button_row (st->config->default_drive->name, NULL, "Edit…", G_CALLBACK (on_edit_default), NULL));

  g = group (page, "Drive configurations", st->config->drives->len ? NULL : "No drives configured yet. Select a drive and choose “Set Up This Drive…”.");
  g_ptr_array_add (groups, g);
  for (guint i = 0; i < st->config->drives->len; i++)
    {
      BroDriveConfig *d = st->config->drives->pdata[i];
      GtkWidget *row = adw_action_row_new ();
      GtkWidget *edit = gtk_button_new_with_label ("Edit…");
      GtkWidget *dup = bro_icon_button ("edit-copy-symbolic", "Duplicate");
      g_autofree char *name = g_markup_escape_text (d->name, -1);
      g_autofree char *sub = g_markup_escape_text (*d->match_drive_name ? d->match_drive_name : d->match_device, -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      gtk_widget_set_valign (edit, GTK_ALIGN_CENTER);
      g_object_set_data_full (G_OBJECT (edit), "id", g_strdup (d->id), g_free);
      g_object_set_data_full (G_OBJECT (dup), "id", g_strdup (d->id), g_free);
      g_signal_connect (edit, "clicked", G_CALLBACK (on_edit_drive), NULL);
      g_signal_connect (dup, "clicked", G_CALLBACK (on_duplicate_drive), page);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), dup);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), edit);
      adw_preferences_group_add (g, row);
    }

  g = group (page, "Presets", "Presets store everything except a drive's name and identification. Apply them from a drive's configuration.");
  g_ptr_array_add (groups, g);
  for (guint i = 0; i < st->config->presets->len; i++)
    {
      BroPreset *p = st->config->presets->pdata[i];
      GtkWidget *row = adw_entry_row_new ();
      GtkWidget *del = bro_icon_button ("user-trash-symbolic", "Delete preset");
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Preset name");
      gtk_editable_set_text (GTK_EDITABLE (row), p->name);
      g_object_set_data (G_OBJECT (row), "preset", p);
      g_signal_connect (row, "changed", G_CALLBACK (preset_renamed), NULL);
      g_object_set_data (G_OBJECT (del), "preset", p);
      g_signal_connect (del, "clicked", G_CALLBACK (on_delete_preset), page);
      adw_entry_row_add_suffix (ADW_ENTRY_ROW (row), del);
      adw_preferences_group_add (g, row);
    }

  g = group (page, "Import / export", "Exports are plain JSON and work with the macOS and Windows versions of Bromelia.");
  g_ptr_array_add (groups, g);
  adw_preferences_group_add (g, button_row ("Export drives and presets", NULL, "Export…", G_CALLBACK (on_export), NULL));
  adw_preferences_group_add (g, button_row ("Import drives and presets", NULL, "Import…", G_CALLBACK (on_import_drives), page));
}

static AdwPreferencesPage *
drives_page (void)
{
  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), "Drives");
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "drive-optical-symbolic");
  drives_rebuild (ADW_PREFERENCES_PAGE (page));
  return ADW_PREFERENCES_PAGE (page);
}

/* ---- registration ---- */

static void
register_done (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *label = data;
  g_autofree char *text = bro_state_register_key_finish (BRO_STATE (src), res);
  gtk_label_set_text (GTK_LABEL (label), text);
  g_object_unref (label);
}

static void
on_register (GtkButton *b, GtkWidget *label)
{
  BroState *st = bro_app_state ();
  if (!st->config->registration_key || !*st->config->registration_key)
    {
      gtk_label_set_text (GTK_LABEL (label), "Enter a key first.");
      return;
    }
  gtk_label_set_text (GTK_LABEL (label), "Registering…");
  bro_state_register_key (st, st->config->registration_key, register_done, g_object_ref (label));
}

static void
beta_done (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *label = data;
  g_autofree char *text = bro_state_install_beta_key_finish (BRO_STATE (src), res);
  gtk_label_set_text (GTK_LABEL (label), text);
  g_object_unref (label);
}

static void
on_beta_key (GtkButton *b, GtkWidget *label)
{
  gtk_label_set_text (GTK_LABEL (label), "Getting the current beta key…");
  bro_state_install_beta_key (bro_app_state (), beta_done, g_object_ref (label));
}

static void
on_key_changed (GtkEditable *e, gpointer data)
{
  BroState *st = bro_app_state ();
  g_free (st->config->registration_key);
  st->config->registration_key = g_strdup (gtk_editable_get_text (e));
  bro_state_config_changed (st);
}

static void
open_uri (GtkButton *b, const char *uri)
{
  g_autoptr (GtkUriLauncher) l = gtk_uri_launcher_new (uri);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (b));
  gtk_uri_launcher_launch (l, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
}

static AdwPreferencesPage *
registration_page (void)
{
  BroState *st = bro_app_state ();
  GtkWidget *page = adw_preferences_page_new ();
  AdwPreferencesGroup *g;
  GtkWidget *key = adw_password_entry_row_new ();
  GtkWidget *result = bro_caption ("");
  g_autoptr (GHashTable) installed = bro_makemkv_installed_settings ();
  const char *desc = g_hash_table_lookup (installed, "app_Key")
                       ? "Empty: the key MakeMKV is registered with is used. Otherwise this key is passed to makemkvcon for every drive."
                       : "No key found in MakeMKV's settings. Without a key MakeMKV runs in evaluation / beta mode.";
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), "Registration");
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "dialog-password-symbolic");
  g = group (ADW_PREFERENCES_PAGE (page), "MakeMKV registration", desc);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (key), "Registration key");
  gtk_editable_set_text (GTK_EDITABLE (key), st->config->registration_key);
  g_signal_connect (key, "changed", G_CALLBACK (on_key_changed), NULL);
  adw_preferences_group_add (g, key);
  adw_preferences_group_add (g, button_row ("Register key with MakeMKV", "Runs makemkvcon reg so the MakeMKV app uses the key too", "Register",
                                            G_CALLBACK (on_register), result));
  adw_preferences_group_add (g, button_row ("Current beta key", "Reads the free beta key from MakeMKV's forum (needs curl) and registers it",
                                            "Get", G_CALLBACK (on_beta_key), result));
  {
    GtkWidget *row = adw_action_row_new ();
    GtkWidget *buy = gtk_button_new_with_label ("Get a Key…");
    GtkWidget *beta = gtk_button_new_with_label ("Beta Key Forum Page…");
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Keys");
    gtk_widget_set_valign (buy, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (beta, GTK_ALIGN_CENTER);
    g_signal_connect (buy, "clicked", G_CALLBACK (open_uri), (gpointer) "https://www.makemkv.com/buy/");
    g_signal_connect (beta, "clicked", G_CALLBACK (open_uri), (gpointer) BRO_BETA_KEY_URL);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), buy);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), beta);
    adw_preferences_group_add (g, row);
  }
  adw_preferences_group_add (g, bro_switch_row ("Update the beta key automatically", "At startup and when it expires. Only a beta key "
                                                "(or no key) is replaced; a purchased key is never changed.",
                                                &st->config->auto_update_beta_key, changed_cb, NULL));
  gtk_widget_set_margin_top (result, 6);
  adw_preferences_group_add (g, result);
  return ADW_PREFERENCES_PAGE (page);
}

/* ---- services ---- */

static const char *const provider_labels[] = { "Off", "The Movie Database (TMDb)", "OMDb (IMDb data)", NULL };

static void
on_secret_changed (GtkEditable *e, char **field)
{
  g_free (*field);
  *field = g_strdup (gtk_editable_get_text (e));
  bro_state_config_changed (bro_app_state ());
}

static GtkWidget *
secret_row (const char *title, char **field)
{
  GtkWidget *row = adw_password_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  gtk_editable_set_text (GTK_EDITABLE (row), *field ? *field : "");
  g_signal_connect (row, "changed", G_CALLBACK (on_secret_changed), field);
  return row;
}

typedef struct {
  AdwPreferencesGroup *group;
  GPtrArray *rows;
  GtkWidget *result;
  GtkWidget *web_status;
} Services;

static void notifications_rebuild (Services *sv);

static void
on_remove_target (GtkButton *b, Services *sv)
{
  BroState *st = bro_app_state ();
  g_ptr_array_remove (st->config->notifications, g_object_get_data (G_OBJECT (b), "target"));
  bro_state_config_changed (st);
  notifications_rebuild (sv);
}

static void
on_add_target (GtkButton *b, Services *sv)
{
  BroState *st = bro_app_state ();
  g_ptr_array_add (st->config->notifications, bro_notification_target_new ());
  bro_state_config_changed (st);
  notifications_rebuild (sv);
}

typedef struct {
  GPtrArray *targets; /* copies */
  GString *log;
} TestSend;

static void
test_log (const char *text, gpointer data)
{
  TestSend *t = data;
  g_string_append_printf (t->log, "%s%s", t->log->len ? "\n" : "", text);
}

static void
test_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  TestSend *t = task_data;
  bro_notifications_send (t->targets, "Bromelia test", "Notifications work.", "success", test_log, t);
  g_task_return_pointer (task, g_strdup (t->log->len ? t->log->str : "Sent."), g_free);
}

static void
test_free (TestSend *t)
{
  g_ptr_array_unref (t->targets);
  g_string_free (t->log, TRUE);
  g_free (t);
}

static void
test_done (GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *label = data;
  g_autofree char *text = g_task_propagate_pointer (G_TASK (res), NULL);
  gtk_label_set_text (GTK_LABEL (label), text ? text : "");
  g_object_unref (label);
}

static void
on_test_targets (GtkButton *b, Services *sv)
{
  BroState *st = bro_app_state ();
  TestSend *t = g_new0 (TestSend, 1);
  GTask *task = g_task_new (NULL, NULL, test_done, g_object_ref (sv->result));
  t->targets = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_notification_target_free);
  t->log = g_string_new (NULL);
  for (guint i = 0; i < st->config->notifications->len; i++)
    {
      BroNotificationTarget *src = st->config->notifications->pdata[i], *copy = bro_notification_target_new ();
      g_free (copy->url);
      copy->url = g_strdup (src->url);
      copy->enabled = src->enabled;
      g_ptr_array_add (t->targets, copy);
    }
  gtk_label_set_text (GTK_LABEL (sv->result), "Sending…");
  g_task_set_task_data (task, t, (GDestroyNotify) test_free);
  g_task_run_in_thread (task, test_thread);
  g_object_unref (task);
}

static void
notifications_rebuild (Services *sv)
{
  BroState *st = bro_app_state ();
  for (guint i = 0; i < sv->rows->len; i++)
    adw_preferences_group_remove (sv->group, sv->rows->pdata[i]);
  g_ptr_array_set_size (sv->rows, 0);
  for (guint i = 0; i < st->config->notifications->len; i++)
    {
      BroNotificationTarget *t = st->config->notifications->pdata[i];
      GtkWidget *exp = adw_expander_row_new ();
      GtkWidget *remove = gtk_button_new_from_icon_name ("user-trash-symbolic");
      g_autofree char *title = g_markup_escape_text (*t->url ? t->url : "New notification", -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (exp), title);
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (exp), t->enabled ? (t->only_problems ? "Only problems" : "Every job") : "Off");
      gtk_widget_add_css_class (remove, "flat");
      gtk_widget_set_valign (remove, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (remove, "Remove");
      g_object_set_data (G_OBJECT (remove), "target", t);
      g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_target), sv);
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), remove);
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp),
                                bro_entry_row ("URL", &t->url, "https://discord.com/api/webhooks/…, ntfy://topic, tgram://…", changed_cb, NULL));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_switch_row ("Enabled", NULL, &t->enabled, changed_cb, NULL));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_switch_row ("Only jobs that didn't succeed", NULL, &t->only_problems, changed_cb, NULL));
      adw_preferences_group_add (sv->group, exp);
      g_ptr_array_add (sv->rows, exp);
    }
}

static void
services_free (Services *sv)
{
  g_ptr_array_unref (sv->rows);
  g_free (sv);
}

static void
refresh_web_status (Services *sv)
{
  BroState *st = bro_app_state ();
  const BroWebUIConfig *w = &st->config->web_ui;
  const char *err = bro_state_web_error (st);
  g_autofree char *text = NULL;
  if (w->enabled && !err)
    {
      const char *host = g_str_equal (w->address, "0.0.0.0") ? "localhost" : w->address;
      text = g_strdup_printf ("http://%s:%d/%s%s", host, w->port, *w->token ? "?token=" : "", w->token);
    }
  gtk_label_set_text (GTK_LABEL (sv->web_status), err && w->enabled ? err : (text ? text : ""));
  gtk_label_set_selectable (GTK_LABEL (sv->web_status), TRUE);
}

static void
web_changed_cb (gpointer data)
{
  bro_state_config_changed (bro_app_state ());
  refresh_web_status (data);
}

static AdwPreferencesPage *
services_page (void)
{
  BroState *st = bro_app_state ();
  BroAppConfig *c = st->config;
  GtkWidget *page = adw_preferences_page_new ();
  Services *sv = g_new0 (Services, 1);
  AdwPreferencesGroup *g;
  GtkWidget *row;
  sv->rows = g_ptr_array_new ();
  g_object_set_data_full (G_OBJECT (page), "services", sv, (GDestroyNotify) services_free);
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), "Services");
  adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (page), "services");
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "network-workgroup-symbolic");

  g = group (ADW_PREFERENCES_PAGE (page), "Online lookup",
             "Finds the canonical title and year of the name read from the disc, for {name}, {releaseYear}, {tmdb} and {imdb} and for "
             "media server names, and the episode titles ({episodeTitle}). The disc page shows the other results and takes a year or an "
             "id when the best match is wrong. Get a free key at themoviedb.org (Settings → API) or omdbapi.com. Needs curl.");
  adw_preferences_group_add (g, bro_combo_row ("Look up movies and shows", provider_labels, (int *) &c->metadata.provider, changed_cb, NULL));
  adw_preferences_group_add (g, secret_row ("API key", &c->metadata.api_key));
  adw_preferences_group_add (g, bro_entry_row ("Language (TMDb)", &c->metadata.language, "en-US", changed_cb, NULL));
  adw_preferences_group_add (g, bro_switch_row ("Look up episode titles of TV shows", NULL, &c->metadata.episode_titles, changed_cb, NULL));
  adw_preferences_group_add (g, bro_switch_row ("Write .nfo files and the poster in media server libraries",
                                                "Kodi / Jellyfin / Emby: tvshow.nfo or the movie's .nfo, an .nfo per episode, and poster.jpg",
                                                &c->metadata.nfo, changed_cb, NULL));

  g = group (ADW_PREFERENCES_PAGE (page), "Notifications",
             "Sent when a job finishes. Discord and Slack webhooks, ntfy (ntfy://topic or ntfys://host/topic) and any https webhook "
             "(JSON) work directly with curl; other Apprise URLs (Telegram, Pushover, e-mail, …) need the apprise command "
             "(pip install apprise).");
  sv->group = g;
  sv->result = bro_caption ("");
  {
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *add = gtk_button_new_with_label ("Add");
    GtkWidget *test = gtk_button_new_with_label ("Send a Test");
    gtk_widget_add_css_class (add, "flat");
    gtk_widget_add_css_class (test, "flat");
    g_signal_connect (add, "clicked", G_CALLBACK (on_add_target), sv);
    g_signal_connect (test, "clicked", G_CALLBACK (on_test_targets), sv);
    gtk_box_append (GTK_BOX (box), add);
    gtk_box_append (GTK_BOX (box), test);
    adw_preferences_group_set_header_suffix (g, box);
  }
  notifications_rebuild (sv);
  g = group (ADW_PREFERENCES_PAGE (page), NULL, NULL);
  adw_preferences_group_add (g, sv->result);

  g = group (ADW_PREFERENCES_PAGE (page), "Web page",
             "Shows the drives, jobs and background steps and lets you rip, eject, close trays and cancel from a browser. "
             "127.0.0.1 keeps it on this computer; 0.0.0.0 opens it to your network and needs a token.");
  sv->web_status = bro_caption ("");
  adw_preferences_group_add (g, bro_switch_row ("Serve a web page", NULL, &c->web_ui.enabled, web_changed_cb, sv));
  adw_preferences_group_add (g, bro_entry_row ("Address", &c->web_ui.address, "127.0.0.1", web_changed_cb, sv));
  adw_preferences_group_add (g, bro_spin_row ("Port", NULL, &c->web_ui.port, 1, 65535, web_changed_cb, sv));
  row = secret_row ("Token (required for other computers)", &c->web_ui.token);
  g_signal_connect_swapped (row, "changed", G_CALLBACK (refresh_web_status), sv);
  adw_preferences_group_add (g, row);
  gtk_widget_set_margin_top (sv->web_status, 6);
  adw_preferences_group_add (g, sv->web_status);
  refresh_web_status (sv);
  return ADW_PREFERENCES_PAGE (page);
}

void
bro_preferences_present (GtkWidget *parent)
{
  BroState *st = bro_app_state ();
  AdwDialog *dialog = adw_preferences_dialog_new ();
  GtkWidget *catalog = bro_catalog_page_new (st->config->global_settings, st->config->global_settings, FALSE, changed_cb, NULL);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), general_page ());
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), ADW_PREFERENCES_PAGE (catalog));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), drives_page ());
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), ADW_PREFERENCES_PAGE (bro_plugins_page_new (st->config->plugins, changed_cb, NULL)));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), services_page ());
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), registration_page ());
  adw_dialog_set_content_width (dialog, 760);
  adw_dialog_set_content_height (dialog, 720);
  if (g_getenv ("BROMELIA_SNAPSHOT_PAGE")) /* developer aid, see main.c */
    adw_preferences_dialog_set_visible_page_name (ADW_PREFERENCES_DIALOG (dialog), g_getenv ("BROMELIA_SNAPSHOT_PAGE"));
  adw_dialog_present (dialog, parent);
}
