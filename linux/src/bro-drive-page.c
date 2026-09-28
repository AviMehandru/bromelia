/* bro-drive-page.c — a drive (or an opened image / folder): header, active job, title browser, rip actions. */
#include "bro-pages.h"
#include "bro-identity.h"
#include "bro-logic.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  char *tag;
  gboolean is_source;
  char *id;
  char *lane;
  BroSession *session;
  GtkWidget *root;

  GtkWidget *icon, *title, *subtitle, *status, *chips;
  GtkWidget *open_btn, *rip_btn, *eject_btn, *configure_btn, *close_btn, *config_dropdown;
  GtkWidget *job_box;
  BroJob *shown_job;

  GtkWidget *stack;
  GtkWidget *empty_status, *empty_button;
  GtkWidget *loading_label, *loading_progress, *loading_log_box;
  GtkWidget *error_status, *error_log_box;
  GtkWidget *titles_list, *info_list;
  GtkWidget *action_bar, *summary, *output_preview, *mkv_btn, *backup_btn, *folder_btn;
  GtkWidget *identity_bar, *name_entry, *kind_dropdown, *first_spin, *format_label, *sample_label;

  BroDiscInfo *shown_info;
  GHashTable *title_checks; /* int -> GtkCheckButton* */
  GHashTable *title_rows;   /* int -> AdwExpanderRow* */
  gboolean syncing;
  gulong session_changed_id, session_selection_id;
} Page;

static void refresh_all (Page *p);
static void refresh_header (Page *p);
static void refresh_disc (Page *p);
static void sync_checks (Page *p);
static void show_info (Page *p, int title, int track);
static void build_identity (Page *p);
static BroIdentity *session_identity (Page *p, gboolean overrides);

static BroState *st (void) { return bro_app_state (); }

static void
page_free (Page *p)
{
  if (p->session)
    {
      if (p->session_changed_id) g_signal_handler_disconnect (p->session, p->session_changed_id);
      if (p->session_selection_id) g_signal_handler_disconnect (p->session, p->session_selection_id);
      g_object_unref (p->session);
    }
  g_clear_object (&p->shown_job);
  if (p->shown_info) bro_disc_info_unref (p->shown_info);
  g_hash_table_unref (p->title_checks);
  g_hash_table_unref (p->title_rows);
  g_free (p->tag);
  g_free (p->id);
  g_free (p->lane);
  g_free (p);
}

static BroDriveItem *
current_item (Page *p)
{
  return p->is_source ? NULL : bro_state_drive_item (st (), p->id);
}

static BroDriveConfig *
current_config (Page *p)
{
  if (p->session)
    return bro_state_session_config (st (), p->session);
  {
    BroDriveItem *it = current_item (p);
    BroDriveConfig *c = it && it->config ? it->config : st ()->config->default_drive;
    bro_drive_item_free (it);
    return c;
  }
}

static void on_session_changed (BroSession *s, Page *p) { refresh_disc (p); refresh_header (p); }
static void on_selection_changed (BroSession *s, Page *p) { sync_checks (p); }

static void
attach_session (Page *p, BroSession *s)
{
  if (s == p->session)
    return;
  if (p->session)
    {
      g_signal_handler_disconnect (p->session, p->session_changed_id);
      g_signal_handler_disconnect (p->session, p->session_selection_id);
      g_clear_object (&p->session);
    }
  g_clear_pointer (&p->shown_info, bro_disc_info_unref);
  if (s)
    {
      p->session = g_object_ref (s);
      p->session_changed_id = g_signal_connect (s, "changed", G_CALLBACK (on_session_changed), p);
      p->session_selection_id = g_signal_connect (s, "selection-changed", G_CALLBACK (on_selection_changed), p);
    }
}

static void
resolve (Page *p)
{
  if (p->is_source)
    attach_session (p, bro_state_session (st (), p->id));
  else
    {
      BroDriveItem *it = current_item (p);
      g_free (p->lane);
      p->lane = g_strdup (it ? it->lane : "");
      attach_session (p, it && it->entry ? bro_state_session (st (), it->lane) : NULL);
      bro_drive_item_free (it);
    }
}

/* ---- header ---- */

static void
clear_box (GtkWidget *box)
{
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (box)))
    gtk_widget_unparent (c);
}

static void
add_chip (Page *p, const char *text, const char *cls)
{
  gtk_box_append (GTK_BOX (p->chips), bro_tag (text, cls));
}

static const char *
lane_of (Page *p)
{
  return p->session ? p->session->id : (p->lane ? p->lane : "");
}

static void
refresh_header (Page *p)
{
  BroDriveConfig *cfg = current_config (p);
  BroJob *job = bro_state_active_job (st (), lane_of (p));
  gboolean busy = job && job->state == BRO_JOB_RUNNING;
  const BroTitleSelection *r = &cfg->rip.titles;
  g_autofree char *rule = NULL;

  clear_box (p->chips);
  if (p->is_source)
    {
      g_autofree char *name = p->session ? bro_source_display_name (p->session->source) : g_strdup ("Source");
      g_autofree char *arg = p->session ? bro_source_info_argument (p->session->source) : g_strdup ("");
      gtk_label_set_text (GTK_LABEL (p->title), p->session && p->session->info ? bro_disc_info_name (p->session->info) : name);
      gtk_label_set_text (GTK_LABEL (p->subtitle), arg);
      gtk_label_set_text (GTK_LABEL (p->status), "");
      gtk_image_set_from_icon_name (GTK_IMAGE (p->icon), p->session && p->session->source->kind == BRO_SOURCE_FOLDER ? "folder-symbolic" : "media-optical-symbolic");
      gtk_button_set_label (GTK_BUTTON (p->open_btn), "Reload");
      gtk_widget_set_visible (p->rip_btn, FALSE);
      gtk_widget_set_visible (p->eject_btn, FALSE);
      gtk_widget_set_visible (p->configure_btn, FALSE);
      gtk_widget_set_visible (p->close_btn, TRUE);
      gtk_widget_set_visible (p->config_dropdown, TRUE);
      gtk_widget_set_visible (p->backup_btn, FALSE);
      /* Configuration choice. */
      {
        BroAppConfig *c = st ()->config;
        g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
        guint sel = 0;
        gtk_string_list_append (names, c->default_drive->name);
        for (guint i = 0; i < c->drives->len; i++)
          {
            BroDriveConfig *d = c->drives->pdata[i];
            gtk_string_list_append (names, d->name);
            if (p->session && g_str_equal (d->id, p->session->config_id))
              sel = i + 1;
          }
        p->syncing = TRUE;
        gtk_drop_down_set_model (GTK_DROP_DOWN (p->config_dropdown), G_LIST_MODEL (names));
        gtk_drop_down_set_selected (GTK_DROP_DOWN (p->config_dropdown), sel);
        p->syncing = FALSE;
      }
    }
  else
    {
      BroDriveItem *it = current_item (p);
      BroDriveEntry *e = it ? it->entry : NULL;
      g_autofree char *name = it ? bro_drive_item_name (it) : g_strdup ("Drive");
      gboolean inserted = e && e->state == BRO_DRIVE_INSERTED;
      gtk_label_set_text (GTK_LABEL (p->title), name);
      if (e)
        {
          g_autofree char *where = *e->device ? g_strdup (e->device) : g_strdup_printf ("disc:%d", e->index);
          g_autofree char *disc = inserted ? (*e->disc_name ? g_strdup_printf (" · %s (%s)", e->disc_name, bro_disc_type_name (e->flags))
                                                            : g_strdup_printf (" · %s", bro_disc_type_name (e->flags)))
                                           : g_strdup ("");
          g_autofree char *status = g_strdup_printf ("%s · %s%s", where, bro_drive_state_name (e->state), disc);
          gtk_label_set_text (GTK_LABEL (p->subtitle), e->drive_name);
          gtk_label_set_text (GTK_LABEL (p->status), status);
        }
      else
        {
          g_autofree char *waiting = g_strdup_printf ("Waiting for a drive matching “%s”", it && it->config ? it->config->match_drive_name : "");
          gtk_label_set_text (GTK_LABEL (p->subtitle), waiting);
          gtk_label_set_text (GTK_LABEL (p->status), "Disconnected");
        }
      gtk_image_set_from_icon_name (GTK_IMAGE (p->icon), inserted ? "media-optical-symbolic" : "drive-optical-symbolic");
      gtk_widget_set_sensitive (p->open_btn, inserted && !busy && !(p->session && p->session->loading));
      gtk_widget_set_sensitive (p->rip_btn, inserted && !job);
      gtk_widget_set_sensitive (p->eject_btn, e && !busy);
      gtk_button_set_label (GTK_BUTTON (p->configure_btn), it && !it->config && e ? "Set Up This Drive…" : "Configure…");
      gtk_widget_set_visible (p->close_btn, FALSE);
      gtk_widget_set_visible (p->config_dropdown, FALSE);
      gtk_widget_set_visible (p->backup_btn, TRUE);
      if (it && !it->config && e)
        add_chip (p, "Default configuration", "warning");
      bro_drive_item_free (it);
    }

  add_chip (p, bro_rip_mode_short (cfg->rip.mode), NULL);
  switch (r->strategy)
    {
    case BRO_STRATEGY_ALL: rule = g_strdup ("All titles"); break;
    case BRO_STRATEGY_LONGEST: rule = r->longest_count == 1 ? g_strdup ("Main feature") : g_strdup_printf ("Longest %d", r->longest_count); break;
    case BRO_STRATEGY_INDICES: rule = g_strdup_printf ("Titles %s", r->index_pattern); break;
    default: rule = g_strdup ("Manual titles"); break;
    }
  add_chip (p, rule, "purple");
  if (cfg->profile.mode == BRO_PROFILE_GENERATED)
    {
      g_autofree char *prof = g_strdup_printf ("Profile: %s", cfg->profile.generated.name);
      add_chip (p, prof, "teal");
    }
  else
    add_chip (p, cfg->profile.mode == BRO_PROFILE_MAKEMKV_DEFAULT ? "Default profile" : "Custom profile", "teal");
  if (g_hash_table_size (cfg->settings))
    {
      g_autofree char *s = g_strdup_printf ("%u setting override(s)", g_hash_table_size (cfg->settings));
      add_chip (p, s, NULL);
    }
  {
    int steps = 0;
    for (guint i = 0; i < cfg->post_process->len; i++)
      steps += ((BroPostStep *) cfg->post_process->pdata[i])->enabled;
    if (steps)
      {
        g_autofree char *s = g_strdup_printf ("%d post-process step(s)", steps);
        add_chip (p, s, "pink");
      }
  }
  if (cfg->automation.auto_rip_on_insert)
    add_chip (p, "Auto-rip", "warning");
}

/* ---- job ---- */

static void
refresh_job (Page *p)
{
  BroJob *job = bro_state_recent_job (st (), lane_of (p));
  if (job == p->shown_job)
    return;
  clear_box (p->job_box);
  g_set_object (&p->shown_job, job);
  if (job)
    gtk_box_append (GTK_BOX (p->job_box), bro_job_card_new (job));
  gtk_widget_set_visible (p->job_box, job != NULL);
}

/* ---- titles ---- */

static void
on_title_toggled (GtkCheckButton *b, Page *p)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "title"));
  if (!p->syncing && p->session)
    bro_session_set_selected (p->session, index, gtk_check_button_get_active (b));
}

static void
on_track_toggled (GtkCheckButton *b, Page *p)
{
  int title = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "title"));
  int track = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "track"));
  if (!p->syncing && p->session)
    bro_session_set_track (p->session, title, track, gtk_check_button_get_active (b));
}

static void
on_track_activated (AdwActionRow *row, Page *p)
{
  show_info (p, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "title")),
             GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "track")));
}

static void
on_title_info (GtkButton *b, Page *p)
{
  show_info (p, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "title")), -1);
}

static const char *
track_icon (BroTrack *t)
{
  switch (bro_track_kind (t))
    {
    case BRO_TRACK_VIDEO: return "video-x-generic-symbolic";
    case BRO_TRACK_AUDIO: return "audio-x-generic-symbolic";
    case BRO_TRACK_SUBTITLE: return "media-view-subtitles-symbolic";
    default: return "text-x-generic-symbolic";
    }
}

static void
build_tracks (Page *p, BroTitle *t)
{
  AdwExpanderRow *row = g_hash_table_lookup (p->title_rows, GINT_TO_POINTER (t->index));
  GPtrArray *old = g_object_get_data (G_OBJECT (row), "track-rows");
  gboolean custom = bro_session_has_custom_tracks (p->session, t->index);
  GPtrArray *rows = g_ptr_array_new ();
  for (guint i = 0; old && i < old->len; i++)
    adw_expander_row_remove (row, old->pdata[i]);
  for (guint i = 0; i < t->tracks->len; i++)
    {
      BroTrack *tr = t->tracks->pdata[i];
      GtkWidget *tr_row = adw_action_row_new ();
      g_autofree char *summary = bro_track_summary (tr);
      g_autofree char *flags = bro_stream_flags_describe (bro_track_flags (tr));
      g_autofree char *sub = g_strdup_printf ("%s%s%s%s", bro_track_attr (tr, BRO_ATTR_LANG_NAME) ? bro_track_attr (tr, BRO_ATTR_LANG_NAME) : "",
                                              *flags ? " · " : "", flags, bro_track_is_default (tr) ? " · default" : "");
      g_autofree char *escaped = g_markup_escape_text (*summary ? summary : "Track", -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (tr_row), escaped);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (tr_row), sub);
      if (custom)
        {
          GtkWidget *cb = gtk_check_button_new ();
          g_object_set_data (G_OBJECT (cb), "title", GINT_TO_POINTER (t->index));
          g_object_set_data (G_OBJECT (cb), "track", GINT_TO_POINTER (tr->index));
          gtk_check_button_set_active (GTK_CHECK_BUTTON (cb), bro_session_track_selected (p->session, t->index, tr->index));
          g_signal_connect (cb, "toggled", G_CALLBACK (on_track_toggled), p);
          gtk_widget_set_valign (cb, GTK_ALIGN_CENTER);
          adw_action_row_add_prefix (ADW_ACTION_ROW (tr_row), cb);
        }
      adw_action_row_add_prefix (ADW_ACTION_ROW (tr_row), gtk_image_new_from_icon_name (track_icon (tr)));
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (tr_row), TRUE);
      g_object_set_data (G_OBJECT (tr_row), "title", GINT_TO_POINTER (t->index));
      g_object_set_data (G_OBJECT (tr_row), "track", GINT_TO_POINTER (tr->index));
      g_signal_connect (tr_row, "activated", G_CALLBACK (on_track_activated), p);
      adw_expander_row_add_row (row, tr_row);
      g_ptr_array_add (rows, tr_row);
    }
  g_object_set_data_full (G_OBJECT (row), "track-rows", rows, (GDestroyNotify) g_ptr_array_unref);
}

static void
on_customize (GtkButton *b, Page *p)
{
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "title"));
  BroTitle *t = p->session && p->session->info ? bro_disc_info_title (p->session->info, index) : NULL;
  g_autofree char *mkvmerge = bro_state_mkvmerge (st ());
  if (!t)
    return;
  if (bro_session_has_custom_tracks (p->session, index))
    bro_session_reset_tracks (p->session, index);
  else if (!mkvmerge)
    {
      bro_state_set_error (st (), "Choosing individual tracks requires mkvmerge (MKVToolNix).");
      return;
    }
  else
    bro_session_customize_tracks (p->session, index);
  build_tracks (p, t);
  adw_expander_row_set_expanded (g_hash_table_lookup (p->title_rows, GINT_TO_POINTER (index)), TRUE);
  gtk_button_set_label (b, bro_session_has_custom_tracks (p->session, index) ? "Use Profile Tracks" : "Choose Tracks");
}

static void
build_titles (Page *p, BroDiscInfo *info)
{
  GtkWidget *c;
  g_autoptr (BroSelectionResult) res = bro_select_titles (info, &current_config (p)->rip.titles);
  int longest = -1, longest_d = -1;
  if (p->shown_info)
    bro_disc_info_unref (p->shown_info);
  p->shown_info = bro_disc_info_ref (info);
  build_identity (p);
  g_hash_table_remove_all (p->title_checks);
  g_hash_table_remove_all (p->title_rows);
  while ((c = gtk_widget_get_first_child (p->titles_list)))
    gtk_list_box_remove (GTK_LIST_BOX (p->titles_list), c);

  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      if (bro_title_duration (t) > longest_d)
        {
          longest_d = bro_title_duration (t);
          longest = t->index;
        }
    }
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      GtkWidget *row = adw_expander_row_new ();
      GtkWidget *cb = gtk_check_button_new ();
      GtkWidget *info_btn = bro_icon_button ("dialog-information-symbolic", "Show title details");
      GtkWidget *tracks_btn = gtk_button_new_with_label ("Choose Tracks");
      g_autofree char *size = bro_title_attr (t, BRO_ATTR_DISK_SIZE) ? g_strdup (bro_title_attr (t, BRO_ATTR_DISK_SIZE))
                                                                   : bro_format_bytes (bro_title_size (t));
      g_autofree char *head = g_strdup_printf ("Title %d    %s    %d ch    %s%s", t->index, bro_title_str (t, BRO_ATTR_DURATION),
                                               bro_title_chapters (t), size, t->index == longest ? "    ★ longest" : "");
      GString *sub = g_string_new (NULL);
      const char *name = bro_title_str (t, BRO_ATTR_NAME);
      if (*name) g_string_append (sub, name);
      if (bro_title_source_id (t) >= 0) g_string_append_printf (sub, "%ssource #%d", sub->len ? " · " : "", bro_title_source_id (t));
      if (*bro_title_str (t, BRO_ATTR_SOURCE_FILE_NAME)) g_string_append_printf (sub, " · %s", bro_title_str (t, BRO_ATTR_SOURCE_FILE_NAME));
      if (*bro_title_str (t, BRO_ATTR_OUTPUT_FILE_NAME)) g_string_append_printf (sub, " · → %s", bro_title_str (t, BRO_ATTR_OUTPUT_FILE_NAME));
      {
        g_autofree char *rule_text = g_strdup_printf (" · rules: %s", bro_selection_result_reason (res, t->index));
        g_string_append (sub, rule_text);
      }
      g_autofree char *sub_escaped = g_markup_escape_text (sub->str, -1);
      g_string_free (sub, TRUE);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), head);
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (row), sub_escaped);
      g_object_set_data (G_OBJECT (cb), "title", GINT_TO_POINTER (t->index));
      gtk_widget_set_valign (cb, GTK_ALIGN_CENTER);
      g_signal_connect (cb, "toggled", G_CALLBACK (on_title_toggled), p);
      adw_expander_row_add_prefix (ADW_EXPANDER_ROW (row), cb);
      if (bro_selection_result_is_selected (res, t->index))
        adw_expander_row_add_suffix (ADW_EXPANDER_ROW (row), gtk_image_new_from_icon_name ("emblem-ok-symbolic"));
      g_object_set_data (G_OBJECT (info_btn), "title", GINT_TO_POINTER (t->index));
      g_signal_connect (info_btn, "clicked", G_CALLBACK (on_title_info), p);
      g_object_set_data (G_OBJECT (tracks_btn), "title", GINT_TO_POINTER (t->index));
      gtk_widget_set_valign (tracks_btn, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (tracks_btn, "flat");
      gtk_widget_set_tooltip_text (tracks_btn, "Choose tracks manually instead of using the profile's selection rule (requires mkvmerge)");
      g_signal_connect (tracks_btn, "clicked", G_CALLBACK (on_customize), p);
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (row), tracks_btn);
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (row), info_btn);
      gtk_list_box_append (GTK_LIST_BOX (p->titles_list), row);
      g_hash_table_insert (p->title_checks, GINT_TO_POINTER (t->index), cb);
      g_hash_table_insert (p->title_rows, GINT_TO_POINTER (t->index), row);
      build_tracks (p, t);
    }
  sync_checks (p);
  show_info (p, -1, -1);
}

static char *
output_preview (Page *p)
{
  BroDriveConfig *cfg = current_config (p);
  g_autoptr (GHashTable) v = bro_template_values_new ();
  const char *root;
  g_autofree char *root_x = NULL;
  g_autofree char *rel = NULL;
  if (p->session && p->session->output_override && *p->session->output_override)
    return g_strdup_printf ("→ %s  (one-off folder)", p->session->output_override);
  bro_template_values_set (v, "disc", p->session && p->session->info && *bro_disc_info_name (p->session->info) ? bro_disc_info_name (p->session->info) : "Disc");
  bro_template_values_set (v, "type", bro_disc_info_type_token (p->session ? p->session->info : NULL));
  bro_template_values_set (v, "drive", cfg->name);
  bro_template_values_set (v, "job", "xxxxxxxx");
  bro_template_values_set (v, "volume", "");
  if (p->session && p->session->info)
    {
      BroIdentity *id = session_identity (p, TRUE);
      bro_identity_template_values (id, v, "Rip");
      bro_identity_free (id);
    }
  root = bro_app_config_output_root (st ()->config, cfg);
  root_x = g_str_has_prefix (root, "~") ? g_build_filename (g_get_home_dir (), root + 1, NULL) : g_strdup (root);
  rel = bro_template_render_path (cfg->output.folder_template, v);
  return *rel ? g_strdup_printf ("→ %s/%s", root_x, rel) : g_strdup_printf ("→ %s", root_x);
}

static BroIdentity *
session_identity (Page *p, gboolean overrides)
{
  BroSession *s = p->session;
  return bro_identity_resolve (s->info, s->info ? bro_disc_info_name (s->info) : "", s->disc_flags, -1, FALSE,
                               overrides ? s->media_name : "", overrides ? s->media_kind : -1, 0);
}

static char *
sample_file_name (Page *p)
{
  BroDriveConfig *cfg = current_config (p);
  g_autofree char *tmpl = g_strstrip (g_strdup (cfg->output.file_name_template));
  g_autoptr (GHashTable) v = bro_template_values_new ();
  BroDiscInfo *info = p->session->info;
  BroIdentity *id;
  BroTitle *title = NULL;
  g_autofree char *rel = NULL;
  if (!*tmpl)
    return g_strdup ("MakeMKV's file names");
  id = session_identity (p, TRUE);
  bro_identity_template_values (id, v, "Rip");
  bro_template_values_set (v, "disc", bro_disc_info_name (info));
  bro_template_values_set (v, "type", bro_disc_info_type_token (info));
  bro_template_values_set (v, "drive", cfg->name);
  bro_template_values_set (v, "job", "xxxxxxxx");
  for (guint i = 0; i < info->titles->len && !title; i++)
    if (g_hash_table_contains (p->session->selected, GINT_TO_POINTER (((BroTitle *) info->titles->pdata[i])->index)))
      title = info->titles->pdata[i];
  if (!title && info->titles->len)
    title = info->titles->pdata[0];
  if (title)
    {
      g_autofree char *track = bro_track_label (title);
      g_autofree char *d = g_strdup_printf ("%d", bro_title_chapters (title));
      bro_template_values_set (v, "track", track);
      bro_template_values_set (v, "title", *bro_title_str (title, BRO_ATTR_NAME) ? bro_title_str (title, BRO_ATTR_NAME) : bro_disc_info_name (info));
      bro_template_values_set (v, "chapters", d);
    }
  if (id->kind == BRO_KIND_TV)
    {
      int first = p->session->first_episode >= 0 ? p->session->first_episode : 1;
      g_autofree char *label = bro_episode_label (first, 2);
      g_autofree char *num = g_strdup_printf ("%d", first);
      bro_template_values_set (v, "episode", label);
      bro_template_values_set (v, "episodeNumber", num);
    }
  bro_identity_free (id);
  rel = bro_template_render_path (tmpl, v);
  return g_strdup_printf ("e.g. %s.mkv", rel);
}

static void
refresh_identity (Page *p)
{
  BroIdentity *id;
  g_autofree char *code = NULL, *sample = NULL, *prev = NULL;
  if (!p->session || !p->session->info)
    return;
  id = session_identity (p, TRUE);
  code = bro_identity_format_code (id);
  gtk_label_set_text (GTK_LABEL (p->format_label), code);
  gtk_widget_set_tooltip_text (p->format_label, bro_format_label (id->format));
  gtk_widget_set_visible (p->first_spin, id->kind == BRO_KIND_TV);
  bro_identity_free (id);
  sample = sample_file_name (p);
  gtk_label_set_text (GTK_LABEL (p->sample_label), sample);
  prev = output_preview (p);
  gtk_label_set_text (GTK_LABEL (p->output_preview), prev);
}

static void
on_media_name (GtkEditable *e, Page *p)
{
  if (p->syncing || !p->session)
    return;
  g_free (p->session->media_name);
  p->session->media_name = g_strdup (gtk_editable_get_text (e));
  refresh_identity (p);
}

static void
on_media_kind (GObject *dd, GParamSpec *pspec, Page *p)
{
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
  if (p->syncing || !p->session)
    return;
  p->session->media_kind = i == 1 ? BRO_KIND_MOVIE : i == 2 ? BRO_KIND_TV : -1;
  refresh_identity (p);
}

static void
on_first_episode (GtkSpinButton *sb, Page *p)
{
  int v = gtk_spin_button_get_value_as_int (sb);
  if (p->syncing || !p->session)
    return;
  p->session->first_episode = v > 0 ? v : -1;
  refresh_identity (p);
}

/* Fills the identity bar from the session (after a disc was opened). */
static void
build_identity (Page *p)
{
  BroIdentity *automatic = session_identity (p, FALSE);
  g_autofree char *auto_label = g_strdup_printf ("Auto (%s)", bro_kind_label (automatic->kind));
  const char *kinds[] = { auto_label, "Movie", "TV show", NULL };
  g_autofree char *tip = g_strdup_printf ("Detected: %s", automatic->reason);
  p->syncing = TRUE;
  gtk_entry_set_placeholder_text (GTK_ENTRY (p->name_entry), automatic->name);
  gtk_editable_set_text (GTK_EDITABLE (p->name_entry), p->session->media_name ? p->session->media_name : "");
  gtk_drop_down_set_model (GTK_DROP_DOWN (p->kind_dropdown), G_LIST_MODEL (gtk_string_list_new (kinds)));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (p->kind_dropdown), p->session->media_kind == BRO_KIND_MOVIE ? 1 : p->session->media_kind == BRO_KIND_TV ? 2 : 0);
  gtk_widget_set_tooltip_text (p->kind_dropdown, tip);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (p->first_spin), p->session->first_episode > 0 ? p->session->first_episode : 0);
  p->syncing = FALSE;
  bro_identity_free (automatic);
  refresh_identity (p);
}

static void
sync_checks (Page *p)
{
  GHashTableIter it;
  gpointer k, v;
  gboolean custom = FALSE;
  BroJob *job;
  if (!p->session)
    return;
  p->syncing = TRUE;
  g_hash_table_iter_init (&it, p->title_checks);
  while (g_hash_table_iter_next (&it, &k, &v))
    gtk_check_button_set_active (GTK_CHECK_BUTTON (v), g_hash_table_contains (p->session->selected, k));
  p->syncing = FALSE;
  g_hash_table_iter_init (&it, p->session->track_selections);
  while (g_hash_table_iter_next (&it, &k, NULL))
    custom |= g_hash_table_contains (p->session->selected, k);
  if (p->session->info)
    {
      g_autofree char *size = bro_format_bytes (bro_session_selected_size (p->session));
      g_autofree char *text = g_strdup_printf ("%u of %u titles · %s%s%s%s", g_hash_table_size (p->session->selected),
                                               p->session->info->titles->len, size, custom ? " · custom tracks" : "",
                                               p->session->libre_drive ? " · " : "", p->session->libre_drive ? p->session->libre_drive : "");
      gtk_label_set_text (GTK_LABEL (p->summary), text);
    }
  {
    g_autofree char *prev = output_preview (p);
    gtk_label_set_text (GTK_LABEL (p->output_preview), prev);
  }
  if (p->session->info && p->sample_label)
    {
      g_autofree char *sample = sample_file_name (p);
      gtk_label_set_text (GTK_LABEL (p->sample_label), sample);
    }
  job = bro_state_active_job (st (), lane_of (p));
  gtk_widget_set_sensitive (p->mkv_btn, g_hash_table_size (p->session->selected) > 0 && !(job && job->state == BRO_JOB_RUNNING));
  gtk_widget_set_sensitive (p->backup_btn, !(job && job->state == BRO_JOB_RUNNING));
}

static void
on_name_changed (GtkEditable *e, Page *p)
{
  int title = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (e), "title"));
  const char *text = gtk_editable_get_text (e);
  if (!p->session)
    return;
  if (*text)
    g_hash_table_replace (p->session->name_overrides, GINT_TO_POINTER (title), g_strdup (text));
  else
    g_hash_table_remove (p->session->name_overrides, GINT_TO_POINTER (title));
}

static void
show_info (Page *p, int title, int track)
{
  GtkWidget *c;
  GHashTable *attrs = NULL;
  g_autofree char *heading = NULL;
  BroDiscInfo *info = p->session ? p->session->info : NULL;

  while ((c = gtk_widget_get_first_child (p->info_list)))
    gtk_list_box_remove (GTK_LIST_BOX (p->info_list), c);
  if (!info)
    return;
  if (title < 0)
    {
      attrs = info->attrs;
      heading = g_strdup ("Disc information");
    }
  else
    {
      BroTitle *t = bro_disc_info_title (info, title);
      if (!t)
        return;
      if (track >= 0)
        {
          for (guint i = 0; i < t->tracks->len; i++)
            if (((BroTrack *) t->tracks->pdata[i])->index == track)
              attrs = ((BroTrack *) t->tracks->pdata[i])->attrs;
          heading = g_strdup_printf ("Title %d · Track %d", title, track);
        }
      else
        {
          GtkWidget *name_row = adw_entry_row_new ();
          attrs = t->attrs;
          heading = g_strdup_printf ("Title %d", title);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_row), "Output file name (overrides the template)");
          gtk_editable_set_text (GTK_EDITABLE (name_row), g_hash_table_lookup (p->session->name_overrides, GINT_TO_POINTER (title)) ?: "");
          g_object_set_data (G_OBJECT (name_row), "title", GINT_TO_POINTER (title));
          g_signal_connect (name_row, "changed", G_CALLBACK (on_name_changed), p);
          gtk_list_box_append (GTK_LIST_BOX (p->info_list), name_row);
        }
    }
  {
    GtkWidget *h = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (h), heading);
    gtk_widget_add_css_class (h, "property");
    gtk_list_box_prepend (GTK_LIST_BOX (p->info_list), h);
  }
  if (!attrs)
    return;
  for (int id = 1; id < BRO_ATTR_MAX; id++)
    {
      const char *value = g_hash_table_lookup (attrs, GINT_TO_POINTER (id));
      GtkWidget *row;
      g_autofree char *flags = NULL;
      g_autofree char *escaped = NULL;
      if (!value || !*value || !bro_attribute_visible (id) || !bro_attribute_name (id))
        continue;
      if (id == BRO_ATTR_STREAM_FLAGS)
        {
          flags = bro_stream_flags_describe (atoi (value));
          if (*flags)
            value = flags;
        }
      row = adw_action_row_new ();
      escaped = g_markup_escape_text (value, -1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), bro_attribute_name (id));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), escaped);
      adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
      gtk_widget_add_css_class (row, "property");
      gtk_list_box_append (GTK_LIST_BOX (p->info_list), row);
    }
}

/* ---- disc area ---- */

static void
refresh_disc (Page *p)
{
  BroSession *s = p->session;
  const char *page;
  if (!s)
    {
      adw_status_page_set_title (ADW_STATUS_PAGE (p->empty_status), "Drive not connected");
      adw_status_page_set_description (ADW_STATUS_PAGE (p->empty_status), "This configuration is used as soon as a matching drive is connected.");
      gtk_widget_set_visible (p->empty_button, FALSE);
      page = "empty";
    }
  else if (s->loading)
    {
      gtk_label_set_text (GTK_LABEL (p->loading_label), *s->operation ? s->operation : "Reading disc…");
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (p->loading_progress), s->progress);
      if (!gtk_widget_get_first_child (p->loading_log_box))
        gtk_box_append (GTK_BOX (p->loading_log_box), bro_log_view_new (G_OBJECT (s), s->log));
      page = "loading";
    }
  else if (s->info)
    {
      clear_box (p->loading_log_box);
      if (s->info != p->shown_info)
        build_titles (p, s->info);
      else
        sync_checks (p);
      page = "browser";
    }
  else if (s->error)
    {
      clear_box (p->loading_log_box);
      adw_status_page_set_description (ADW_STATUS_PAGE (p->error_status), s->error);
      clear_box (p->error_log_box);
      gtk_box_append (GTK_BOX (p->error_log_box), bro_log_view_new (G_OBJECT (s), s->log));
      page = "error";
    }
  else
    {
      BroDriveItem *it = current_item (p);
      gboolean can_open = p->is_source || (it && it->entry && it->entry->state == BRO_DRIVE_INSERTED);
      bro_drive_item_free (it);
      clear_box (p->loading_log_box);
      adw_status_page_set_title (ADW_STATUS_PAGE (p->empty_status), "No disc opened");
      adw_status_page_set_description (ADW_STATUS_PAGE (p->empty_status),
                                       p->is_source ? "Reload to read the image."
                                                    : "Open the disc to browse titles and tracks, or use Rip to apply this drive's rules directly.");
      gtk_widget_set_visible (p->empty_button, can_open);
      page = "empty";
    }
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack), page);
  gtk_widget_set_visible (p->action_bar, g_str_equal (page, "browser"));
  gtk_widget_set_visible (p->identity_bar, g_str_equal (page, "browser"));
}

static void
refresh_all (Page *p)
{
  resolve (p);
  refresh_header (p);
  refresh_job (p);
  refresh_disc (p);
}

/* ---- actions ---- */

static void
on_open_disc (GtkButton *b, Page *p)
{
  if (p->session)
    bro_state_load_disc (st (), p->session);
}

static void
on_cancel_load (GtkButton *b, Page *p)
{
  if (p->session && p->session->cancellable)
    g_cancellable_cancel (p->session->cancellable);
}

static void
on_eject (GtkButton *b, Page *p)
{
  bro_state_eject (st (), lane_of (p));
}

static void
on_configure (GtkButton *b, Page *p)
{
  BroDriveItem *it = current_item (p);
  if (it && it->config)
    bro_config_dialog_present (GTK_WIDGET (b), it->config->id);
  else if (it && it->entry)
    bro_config_dialog_present (GTK_WIDGET (b), bro_state_configure_drive (st (), it->entry)->id);
  bro_drive_item_free (it);
}

static void
on_close_source (GtkButton *b, Page *p)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (b));
  if (p->session)
    bro_state_close_file (st (), p->session);
  if (BRO_IS_WINDOW (root))
    bro_window_navigate (BRO_WINDOW (root), "queue");
}

static void
on_config_selected (GObject *dd, GParamSpec *pspec, Page *p)
{
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
  BroAppConfig *c = st ()->config;
  if (p->syncing || !p->session || sel == GTK_INVALID_LIST_POSITION)
    return;
  g_free (p->session->config_id);
  p->session->config_id = g_strdup (sel == 0 ? c->default_drive->id : ((BroDriveConfig *) c->drives->pdata[sel - 1])->id);
  bro_session_apply_rule (p->session, &current_config (p)->rip.titles);
  g_clear_pointer (&p->shown_info, bro_disc_info_unref);
  refresh_header (p);
  refresh_disc (p);
}

static void
rip (Page *p, BroRipMode mode)
{
  if (p->session)
    bro_state_rip_session (st (), p->session, mode);
  refresh_job (p);
  refresh_header (p);
}

static void on_make_mkv (GtkButton *b, Page *p) { rip (p, BRO_MODE_MKV); }

static void
action_backup (GSimpleAction *a, GVariant *param, gpointer data)
{
  rip (data, (BroRipMode) g_variant_get_int32 (param));
}

static void
action_quick_rip (GSimpleAction *a, GVariant *param, gpointer data)
{
  Page *p = data;
  BroDriveItem *it = current_item (p);
  if (it)
    bro_state_quick_rip (st (), it, param ? g_variant_get_int32 (param) : -1);
  bro_drive_item_free (it);
  refresh_job (p);
  refresh_header (p);
}

static void
action_select (GSimpleAction *a, GVariant *param, gpointer data)
{
  Page *p = data;
  const char *what = g_variant_get_string (param, NULL);
  if (!p->session)
    return;
  if (g_str_equal (what, "all"))
    bro_session_select_all (p->session, TRUE);
  else if (g_str_equal (what, "none"))
    bro_session_select_all (p->session, FALSE);
  else
    bro_session_apply_rule (p->session, &current_config (p)->rip.titles);
}

static void
folder_chosen (const char *path, gpointer data)
{
  Page *p = data;
  if (!p->session)
    return;
  g_free (p->session->output_override);
  p->session->output_override = g_strdup (path);
  gtk_widget_set_tooltip_text (p->folder_btn, "One-off output folder set; click to reset");
  sync_checks (p);
}

static void
on_folder (GtkButton *b, Page *p)
{
  if (!p->session)
    return;
  if (*p->session->output_override)
    {
      g_free (p->session->output_override);
      p->session->output_override = g_strdup ("");
      gtk_widget_set_tooltip_text (p->folder_btn, "Choose a different output folder for this disc");
      sync_checks (p);
      return;
    }
  bro_pick_path (GTK_WIDGET (b), TRUE, "Output folder for this disc", folder_chosen, p);
}

static void
on_state_drives_changed (GtkWidget *root, BroState *s)
{
  Page *p = g_object_get_data (G_OBJECT (root), "bro-page");
  refresh_all (p);
}

static void
on_state_jobs_changed (GtkWidget *root, BroState *s)
{
  Page *p = g_object_get_data (G_OBJECT (root), "bro-page");
  refresh_job (p);
  refresh_header (p);
  if (p->session && p->session->info)
    sync_checks (p);
}

/* ---- construction ---- */

GtkWidget *
bro_drive_page_new (const char *tag)
{
  Page *p = g_new0 (Page, 1);
  GtkWidget *header, *labels, *buttons_row, *buttons_col, *content, *paned, *scroll, *info_scroll, *loading, *error_box, *empty_box;
  GSimpleActionGroup *group = g_simple_action_group_new ();
  static const GActionEntry entries[] = {
    { "backup", action_backup, "i", NULL, NULL, { 0 } },
    { "quick-rip", action_quick_rip, "i", NULL, NULL, { 0 } },
    { "select", action_select, "s", NULL, NULL, { 0 } },
  };
  GMenu *rip_menu = g_menu_new ();
  GMenu *backup_menu = g_menu_new ();
  GMenu *select_menu = g_menu_new ();

  p->tag = g_strdup (tag);
  p->is_source = g_str_has_prefix (tag, "source:");
  p->id = g_strdup (strchr (tag, ':') + 1);
  p->title_checks = g_hash_table_new (g_direct_hash, g_direct_equal);
  p->title_rows = g_hash_table_new (g_direct_hash, g_direct_equal);

  p->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (p->root, 18);
  gtk_widget_set_margin_end (p->root, 18);
  gtk_widget_set_margin_top (p->root, 12);
  gtk_widget_set_margin_bottom (p->root, 12);
  g_action_map_add_action_entries (G_ACTION_MAP (group), entries, G_N_ELEMENTS (entries), p);
  gtk_widget_insert_action_group (p->root, "page", G_ACTION_GROUP (group));
  g_object_unref (group);

  /* Header. */
  header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  p->icon = gtk_image_new_from_icon_name ("drive-optical-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (p->icon), 48);
  gtk_widget_add_css_class (p->icon, "drive-icon");
  gtk_widget_set_valign (p->icon, GTK_ALIGN_START);
  labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (labels, TRUE);
  p->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (p->title), 0);
  gtk_widget_add_css_class (p->title, "title-2");
  p->subtitle = bro_caption ("");
  p->status = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (p->status), 0);
  gtk_widget_add_css_class (p->status, "caption");
  p->chips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (p->chips, 6);
  gtk_box_append (GTK_BOX (labels), p->title);
  gtk_box_append (GTK_BOX (labels), p->subtitle);
  gtk_box_append (GTK_BOX (labels), p->status);
  gtk_box_append (GTK_BOX (labels), p->chips);

  p->open_btn = gtk_button_new_with_label ("Open Disc");
  g_signal_connect (p->open_btn, "clicked", G_CALLBACK (on_open_disc), p);
  for (int m = BRO_MODE_MKV; m <= BRO_MODE_INFO_ONLY; m++)
    {
      g_autofree char *action = g_strdup_printf ("page.quick-rip(%d)", m);
      g_menu_append (rip_menu, bro_rip_mode_label (m), action);
    }
  p->rip_btn = adw_split_button_new ();
  adw_split_button_set_label (ADW_SPLIT_BUTTON (p->rip_btn), "Rip");
  adw_split_button_set_menu_model (ADW_SPLIT_BUTTON (p->rip_btn), G_MENU_MODEL (rip_menu));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (p->rip_btn), "page.quick-rip");
  gtk_actionable_set_action_target (GTK_ACTIONABLE (p->rip_btn), "i", -1);
  gtk_widget_set_tooltip_text (p->rip_btn, "Rip with this drive's configured mode and title rules");
  p->eject_btn = gtk_button_new_with_label ("Eject");
  g_signal_connect (p->eject_btn, "clicked", G_CALLBACK (on_eject), p);
  p->close_btn = gtk_button_new_with_label ("Close");
  g_signal_connect (p->close_btn, "clicked", G_CALLBACK (on_close_source), p);
  p->config_dropdown = gtk_drop_down_new (NULL, NULL);
  gtk_widget_set_tooltip_text (p->config_dropdown, "Configuration used for this image");
  g_signal_connect (p->config_dropdown, "notify::selected", G_CALLBACK (on_config_selected), p);
  p->configure_btn = gtk_button_new_with_label ("Configure…");
  g_signal_connect (p->configure_btn, "clicked", G_CALLBACK (on_configure), p);
  buttons_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (buttons_row), p->config_dropdown);
  gtk_box_append (GTK_BOX (buttons_row), p->open_btn);
  gtk_box_append (GTK_BOX (buttons_row), p->rip_btn);
  gtk_box_append (GTK_BOX (buttons_row), p->eject_btn);
  gtk_box_append (GTK_BOX (buttons_row), p->close_btn);
  buttons_col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (buttons_col), buttons_row);
  gtk_widget_set_halign (p->configure_btn, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (buttons_col), p->configure_btn);
  gtk_box_append (GTK_BOX (header), p->icon);
  gtk_box_append (GTK_BOX (header), labels);
  gtk_box_append (GTK_BOX (header), buttons_col);

  p->job_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Disc area. */
  p->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (p->stack, TRUE);

  empty_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  p->empty_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (p->empty_status), "media-optical-symbolic");
  p->empty_button = gtk_button_new_with_label ("Open Disc");
  gtk_widget_add_css_class (p->empty_button, "pill");
  gtk_widget_add_css_class (p->empty_button, "suggested-action");
  gtk_widget_set_halign (p->empty_button, GTK_ALIGN_CENTER);
  g_signal_connect (p->empty_button, "clicked", G_CALLBACK (on_open_disc), p);
  adw_status_page_set_child (ADW_STATUS_PAGE (p->empty_status), p->empty_button);
  gtk_widget_set_vexpand (p->empty_status, TRUE);
  gtk_box_append (GTK_BOX (empty_box), p->empty_status);
  gtk_stack_add_named (GTK_STACK (p->stack), empty_box, "empty");

  loading = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  p->loading_label = gtk_label_new ("Reading disc…");
  gtk_label_set_xalign (GTK_LABEL (p->loading_label), 0);
  p->loading_progress = gtk_progress_bar_new ();
  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *cancel = gtk_button_new_with_label ("Cancel");
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel_load), p);
    gtk_widget_set_hexpand (p->loading_progress, TRUE);
    gtk_widget_set_valign (p->loading_progress, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (row), p->loading_progress);
    gtk_box_append (GTK_BOX (row), cancel);
    gtk_box_append (GTK_BOX (loading), p->loading_label);
    gtk_box_append (GTK_BOX (loading), row);
  }
  p->loading_log_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (p->loading_log_box, TRUE);
  gtk_box_append (GTK_BOX (loading), p->loading_log_box);
  gtk_stack_add_named (GTK_STACK (p->stack), loading, "loading");

  error_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  p->error_status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (p->error_status), "dialog-warning-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (p->error_status), "Could not open the disc");
  {
    GtkWidget *retry = gtk_button_new_with_label ("Try Again");
    gtk_widget_add_css_class (retry, "pill");
    gtk_widget_set_halign (retry, GTK_ALIGN_CENTER);
    g_signal_connect (retry, "clicked", G_CALLBACK (on_open_disc), p);
    adw_status_page_set_child (ADW_STATUS_PAGE (p->error_status), retry);
  }
  p->error_log_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (p->error_log_box, TRUE);
  gtk_box_append (GTK_BOX (error_box), p->error_status);
  gtk_box_append (GTK_BOX (error_box), p->error_log_box);
  gtk_stack_add_named (GTK_STACK (p->stack), error_box, "error");

  paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  p->titles_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (p->titles_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (p->titles_list, "boxed-list");
  gtk_widget_set_valign (p->titles_list, GTK_ALIGN_START);
  scroll = gtk_scrolled_window_new ();
  {
    GtkWidget *clamp = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_end (clamp, 12);
    gtk_box_append (GTK_BOX (clamp), p->titles_list);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  }
  p->info_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (p->info_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (p->info_list, "boxed-list");
  gtk_widget_set_valign (p->info_list, GTK_ALIGN_START);
  info_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (info_scroll), p->info_list);
  gtk_widget_set_size_request (info_scroll, 300, -1);
  gtk_paned_set_start_child (GTK_PANED (paned), scroll);
  gtk_paned_set_end_child (GTK_PANED (paned), info_scroll);
  gtk_paned_set_resize_end_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
  gtk_stack_add_named (GTK_STACK (p->stack), paned, "browser");

  /* Action bar. */
  p->action_bar = gtk_action_bar_new ();
  g_menu_append (select_menu, "All Titles", "page.select::all");
  g_menu_append (select_menu, "None", "page.select::none");
  g_menu_append (select_menu, "Apply Title Rules", "page.select::rule");
  {
    GtkWidget *select_btn = gtk_menu_button_new ();
    GtkWidget *labels_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_menu_button_set_label (GTK_MENU_BUTTON (select_btn), "Select");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (select_btn), G_MENU_MODEL (select_menu));
    p->summary = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (p->summary), 0);
    p->output_preview = bro_caption ("");
    gtk_label_set_ellipsize (GTK_LABEL (p->output_preview), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_wrap (GTK_LABEL (p->output_preview), FALSE);
    gtk_box_append (GTK_BOX (labels_box), p->summary);
    gtk_box_append (GTK_BOX (labels_box), p->output_preview);
    gtk_action_bar_pack_start (GTK_ACTION_BAR (p->action_bar), select_btn);
    gtk_action_bar_pack_start (GTK_ACTION_BAR (p->action_bar), labels_box);
  }
  for (int m = BRO_MODE_BACKUP; m <= BRO_MODE_BACKUP_THEN_MKV; m++)
    {
      g_autofree char *action = g_strdup_printf ("page.backup(%d)", m);
      g_menu_append (backup_menu, m == BRO_MODE_BACKUP_THEN_MKV ? "Decrypted backup, then MKV of selected titles" : bro_rip_mode_label (m), action);
    }
  p->backup_btn = gtk_menu_button_new ();
  gtk_menu_button_set_label (GTK_MENU_BUTTON (p->backup_btn), "Backup");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (p->backup_btn), G_MENU_MODEL (backup_menu));
  p->mkv_btn = gtk_button_new_with_label ("Make MKV");
  gtk_widget_add_css_class (p->mkv_btn, "suggested-action");
  g_signal_connect (p->mkv_btn, "clicked", G_CALLBACK (on_make_mkv), p);
  p->folder_btn = bro_icon_button ("folder-symbolic", "Choose a different output folder for this disc");
  g_signal_connect (p->folder_btn, "clicked", G_CALLBACK (on_folder), p);
  gtk_action_bar_pack_end (GTK_ACTION_BAR (p->action_bar), p->mkv_btn);
  gtk_action_bar_pack_end (GTK_ACTION_BAR (p->action_bar), p->backup_btn);
  gtk_action_bar_pack_end (GTK_ACTION_BAR (p->action_bar), p->folder_btn);
  g_object_unref (rip_menu);
  g_object_unref (backup_menu);
  g_object_unref (select_menu);

  /* Identity bar: movie / show name, movie or TV, first episode, format code and a sample file name. */
  p->identity_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start (p->identity_bar, 8);
  gtk_widget_set_margin_end (p->identity_bar, 8);
  gtk_widget_set_margin_top (p->identity_bar, 6);
  gtk_widget_set_margin_bottom (p->identity_bar, 2);
  p->name_entry = gtk_entry_new ();
  gtk_widget_set_size_request (p->name_entry, 220, -1);
  gtk_widget_set_tooltip_text (p->name_entry, "Movie or show name used for folder and file names. Empty = the name read from the disc.");
  g_signal_connect (p->name_entry, "changed", G_CALLBACK (on_media_name), p);
  p->kind_dropdown = gtk_drop_down_new (NULL, NULL);
  g_signal_connect (p->kind_dropdown, "notify::selected", G_CALLBACK (on_media_kind), p);
  p->first_spin = gtk_spin_button_new_with_range (0, 9999, 1);
  gtk_widget_set_tooltip_text (p->first_spin, "Number of the first episode on this disc. 0 = read from the disc menus, or 1.");
  g_signal_connect (p->first_spin, "value-changed", G_CALLBACK (on_first_episode), p);
  p->format_label = gtk_label_new ("");
  gtk_widget_add_css_class (p->format_label, "heading");
  p->sample_label = bro_caption ("");
  gtk_label_set_ellipsize (GTK_LABEL (p->sample_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_selectable (GTK_LABEL (p->sample_label), TRUE);
  gtk_widget_set_hexpand (p->sample_label, TRUE);
  gtk_label_set_xalign (GTK_LABEL (p->sample_label), 0);
  gtk_box_append (GTK_BOX (p->identity_bar), p->name_entry);
  gtk_box_append (GTK_BOX (p->identity_bar), p->kind_dropdown);
  gtk_box_append (GTK_BOX (p->identity_bar), p->first_spin);
  gtk_box_append (GTK_BOX (p->identity_bar), p->format_label);
  gtk_box_append (GTK_BOX (p->identity_bar), p->sample_label);

  content = p->root;
  gtk_box_append (GTK_BOX (content), header);
  gtk_box_append (GTK_BOX (content), p->job_box);
  gtk_box_append (GTK_BOX (content), p->stack);
  gtk_box_append (GTK_BOX (content), p->identity_bar);
  gtk_box_append (GTK_BOX (content), p->action_bar);

  g_object_set_data_full (G_OBJECT (p->root), "bro-page", p, (GDestroyNotify) page_free);
  g_signal_connect_object (st (), "drives-changed", G_CALLBACK (on_state_drives_changed), p->root, G_CONNECT_SWAPPED);
  g_signal_connect_object (st (), "jobs-changed", G_CALLBACK (on_state_jobs_changed), p->root, G_CONNECT_SWAPPED);
  refresh_all (p);
  return p->root;
}
