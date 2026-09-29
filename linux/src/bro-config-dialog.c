/* bro-config-dialog.c — per-drive configuration (live-edited, saved automatically) and the settings catalog editor. */
#include "bro-pages.h"
#include "bro-identity.h"
#include "bro-logic.h"

#include <string.h>

static const char *const rip_mode_labels[] = {
  "Rip titles to MKV", "Backup (encrypted, 1:1)", "Backup (decrypted)", "Decrypted backup, then MKV from backup",
  "Scan only (save disc information)", NULL };
static const char *const strategy_labels[] = { "All titles", "Longest title(s)", "Titles matching an index pattern", "Choose manually", NULL };
static const char *const index_base_labels[] = { "MakeMKV title number (0-based)", "Source title ID (playlist / VTS)", NULL };
static const char *const backup_format_labels[] = { "Folder (BDMV / VIDEO_TS)", "ISO image", NULL };
static const char *const conflict_labels[] = { "Add a number (Disc (2))", "Reuse the existing folder", "Skip the job", NULL };
static const char *const run_labels[] = { "When the rip succeeds", "When the rip fails", "Always", NULL };
static const char *const profile_labels[] = { "MakeMKV default profile", "Bromelia profile (edit below)", "Custom profile file (.mmcp.xml)", NULL };
static const char *const lpcm_labels[] = { "Copy as is", "Raw LPCM", "LPCM in WAV container", "FLAC (best compression)", "FLAC (fast compression)", NULL };
static const char *const directio_labels[] = { "MakeMKV default", "Off", "On", NULL };
static const char *const format_mode_labels[] = {
  "Same as the mode above", "Rip titles to MKV", "Backup (encrypted, 1:1)", "Backup (decrypted)", "Decrypted backup, then MKV from backup",
  "Scan only (save disc information)", NULL };
static const char *const layout_labels[] = { "Folder and file name templates", "Plex / Jellyfin / Emby library", NULL };
static const char *const already_archived_labels[] = { "Skip it (eject when done)", "Stop and ask (leave it in the drive)", "Rip it again", NULL };

typedef struct {
  BroDriveConfig *config;
  gboolean is_default;
  BroChangedFunc changed;
  gpointer data;
  AdwPreferencesGroup *preview;
  GtkWidget *preview_rows;
  AdwPreferencesGroup *post_group;
  GPtrArray *post_rows;
  GPtrArray *steps;             /* the step list being edited: the drive's steps or the global plugins */
  GtkWidget *folder_row, *file_row;
  AdwPreferencesDialog *dialog;
  int direct_io_choice;
  int format_choice[BRO_FORMAT_KEY_COUNT]; /* 0 = the drive's mode, else BroRipMode + 1 */
} Ctx;

static void
ctx_changed (gpointer data)
{
  Ctx *c = data;
  if (c->changed)
    c->changed (c->data);
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

static AdwPreferencesPage *
page_new (const char *title, const char *icon)
{
  GtkWidget *p = adw_preferences_page_new ();
  g_autofree char *name = g_ascii_strdown (title, -1);
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (p), title);
  adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (p), name);
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (p), icon);
  return ADW_PREFERENCES_PAGE (p);
}

#define ADD(g, w) adw_preferences_group_add (g, w)

/* ---- general ---- */

static void
use_drive_clicked (GtkButton *b, Ctx *c)
{
  BroDriveEntry *e = g_object_get_data (G_OBJECT (b), "entry");
  GtkWidget *name_row = g_object_get_data (G_OBJECT (b), "name-row");
  GtkWidget *dev_row = g_object_get_data (G_OBJECT (b), "dev-row");
  gtk_editable_set_text (GTK_EDITABLE (name_row), e->drive_name);
  gtk_editable_set_text (GTK_EDITABLE (dev_row), e->device);
}

static AdwPreferencesPage *
general_page (Ctx *c)
{
  AdwPreferencesPage *page = page_new ("General", "drive-optical-symbolic");
  AdwPreferencesGroup *g = group (page, "Drive", c->is_default
                                  ? "The default configuration is used for drives that have not been set up and for disc images. New drive configurations start as a copy of it."
                                  : NULL);
  BroDriveConfig *d = c->config;
  ADD (g, bro_entry_row ("Name", &d->name, NULL, ctx_changed, c));
  if (!c->is_default)
    {
      BroState *st = bro_app_state ();
      GtkWidget *name_row, *dev_row;
      ADD (g, bro_switch_row ("Enabled", NULL, &d->enabled, ctx_changed, c));
      g = group (page, "Drive identification",
                 "The drive name reported by MakeMKV usually includes the serial number, so a configuration follows the drive even when device names change.");
      name_row = bro_entry_row ("Drive name", &d->match_drive_name, NULL, ctx_changed, c);
      dev_row = bro_entry_row ("Device (used when the drive name is empty)", &d->match_device, NULL, ctx_changed, c);
      ADD (g, name_row);
      ADD (g, dev_row);
      for (guint i = 0; i < st->drives->len; i++)
        {
          BroDriveEntry *e = st->drives->pdata[i];
          GtkWidget *row = adw_action_row_new ();
          GtkWidget *btn = gtk_button_new_with_label ("Use");
          g_autofree char *title = g_markup_escape_text (e->drive_name, -1);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), e->device);
          gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
          g_object_set_data (G_OBJECT (btn), "entry", e);
          g_object_set_data (G_OBJECT (btn), "name-row", name_row);
          g_object_set_data (G_OBJECT (btn), "dev-row", dev_row);
          g_signal_connect (btn, "clicked", G_CALLBACK (use_drive_clicked), c);
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), btn);
          ADD (g, row);
        }
    }
  g = group (page, "Automation", NULL);
  ADD (g, bro_switch_row ("Rip automatically when a disc is inserted", NULL, &d->automation.auto_rip_on_insert, ctx_changed, c));
  ADD (g, bro_spin_row ("Start automatic rips after", "seconds (gives time to cancel)", &d->automation.auto_rip_delay_seconds, 0, 3600, ctx_changed, c));
  ADD (g, bro_spin_row ("Wait for the disc to be mounted", "seconds, before an automatic rip (0 = don't wait)", &d->automation.wait_for_mount_seconds, 0, 600, ctx_changed, c));
  {
    GtkWidget *row = bro_combo_row ("A disc archived before", already_archived_labels, (int *) &d->automation.already_archived, ctx_changed, c);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), "Automatic rips; found by its fingerprint in the history or a bromelia.json in the output folder");
    ADD (g, row);
  }
  ADD (g, bro_switch_row ("Eject the disc when the job succeeds", NULL, &d->automation.eject_when_done, ctx_changed, c));
  ADD (g, bro_switch_row ("Eject the disc when the job fails", NULL, &d->automation.eject_on_failure, ctx_changed, c));
  ADD (g, bro_switch_row ("Show a notification when the job finishes", NULL, &d->automation.notify, ctx_changed, c));
  ADD (g, bro_switch_row ("Play a sound", NULL, &d->automation.play_sound, ctx_changed, c));
  return page;
}

/* ---- ripping ---- */

static void
refresh_preview (Ctx *c)
{
  BroState *st = bro_app_state ();
  BroDiscInfo *info = NULL;
  GHashTableIter it;
  gpointer k, v;
  GtkWidget *child;
  if (!c->preview)
    return;
  g_hash_table_iter_init (&it, st->sessions);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      BroSession *s = v;
      if (s->info && g_strcmp0 (s->config_id, c->config->id) == 0)
        info = s->info;
    }
  while ((child = gtk_widget_get_first_child (c->preview_rows)))
    gtk_list_box_remove (GTK_LIST_BOX (c->preview_rows), child);
  gtk_widget_set_visible (GTK_WIDGET (c->preview), info != NULL);
  if (!info)
    return;
  {
    g_autofree char *title = g_strdup_printf ("Preview on “%s”", bro_disc_info_name (info));
    adw_preferences_group_set_title (c->preview, title);
  }
  g_autoptr (BroSelectionResult) r = bro_select_titles (info, &c->config->rip.titles);
  if (r->error)
    {
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), r->error);
      gtk_list_box_append (GTK_LIST_BOX (c->preview_rows), row);
    }
  for (guint i = 0; i < r->decisions->len; i++)
    {
      BroTitleDecision *d = &g_array_index (r->decisions, BroTitleDecision, i);
      BroTitle *t = bro_disc_info_title (info, d->title_index);
      GtkWidget *row = adw_action_row_new ();
      g_autofree char *title = g_strdup_printf ("Title %d   %s   %d ch   %s", d->title_index, bro_title_str (t, BRO_ATTR_DURATION),
                                                bro_title_chapters (t), bro_title_str (t, BRO_ATTR_SOURCE_FILE_NAME));
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), d->reason);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name (d->selected ? "emblem-ok-symbolic" : "list-remove-symbolic"));
      gtk_list_box_append (GTK_LIST_BOX (c->preview_rows), row);
    }
}

static void
ripping_changed (gpointer data)
{
  Ctx *c = data;
  c->config->rip.direct_io = c->direct_io_choice == 0 ? -1 : c->direct_io_choice == 2 ? 1 : 0;
  for (int i = 0; i < BRO_FORMAT_KEY_COUNT; i++)
    c->config->rip.format_modes[i] = c->format_choice[i] - 1;
  ctx_changed (c);
  refresh_preview (c);
}

static AdwPreferencesPage *
ripping_page (Ctx *c)
{
  AdwPreferencesPage *page = page_new ("Ripping", "media-optical-symbolic");
  BroDriveConfig *d = c->config;
  BroTitleSelection *t = &d->rip.titles;
  AdwPreferencesGroup *g = group (page, "What to do with a disc", NULL);
  ADD (g, bro_combo_row ("Mode", rip_mode_labels, (int *) &d->rip.mode, ripping_changed, c));
  ADD (g, bro_combo_row ("Backup format", backup_format_labels, (int *) &d->rip.backup_format, ripping_changed, c));
  ADD (g, bro_switch_row ("Keep the backup after the MKV files are made", "Backup + MKV mode", &d->rip.keep_backup_after_mkv, ripping_changed, c));

  g = group (page, "Modes by disc format", "Used for automatic rips and “Rip”: for example backups of DVDs but MKV files of Blu-rays.");
  {
    static const char *const names[BRO_FORMAT_KEY_COUNT] = { "DVDs", "Blu-rays", "4K Ultra HD Blu-rays" };
    for (int i = 0; i < BRO_FORMAT_KEY_COUNT; i++)
      {
        c->format_choice[i] = d->rip.format_modes[i] >= 0 && d->rip.format_modes[i] < BRO_VIDEO_MODE_COUNT ? d->rip.format_modes[i] + 1 : 0;
        ADD (g, bro_combo_row (names[i], format_mode_labels, &c->format_choice[i], ripping_changed, c));
      }
  }

  g = group (page, "Other discs", "Discs without a DVD or Blu-ray structure, when ripped automatically or with “Rip”. Audio CDs are "
                                  "ripped with cyanrip or abcde, which look the album up in MusicBrainz; data discs are copied byte for "
                                  "byte to an ISO image.");
  ADD (g, bro_switch_row ("Rip audio CDs", NULL, &d->other.rip_audio_cds, ctx_changed, c));
  ADD (g, bro_switch_row ("Save data discs as ISO images", NULL, &d->other.image_data_discs, ctx_changed, c));
  ADD (g, bro_entry_row ("Audio CD command (optional)", &d->other.audio_command,
                         "Runs in the output folder; {device} is the drive. Empty: cyanrip, else abcde", ctx_changed, c));

  g = group (page, "TV episodes", "DVDs often store several episodes in one title. Bromelia reads the disc's menu navigation to find where "
                                  "each episode starts and splits the MKV with mkvmerge, without re-encoding. Episode numbers are read "
                                  "from the menu screens with ffmpeg and tesseract when installed; otherwise enter the first episode "
                                  "number on the disc page, or episodes are numbered from 1.");
  ADD (g, bro_switch_row ("Split “play all” titles of TV shows into episodes", NULL, &d->episodes.split_play_all, ctx_changed, c));
  ADD (g, bro_switch_row ("Keep the unsplit title as well", NULL, &d->episodes.keep_play_all, ctx_changed, c));
  ADD (g, bro_switch_row ("Read episode numbers from the disc menus", NULL, &d->episodes.read_menu_numbers, ctx_changed, c));

  g = group (page, "Titles to rip", NULL);
  ADD (g, bro_combo_row ("Choose", strategy_labels, (int *) &t->strategy, ripping_changed, c));
  ADD (g, bro_spin_row ("Number of longest titles", NULL, &t->longest_count, 1, 99, ripping_changed, c));
  ADD (g, bro_entry_row ("Index pattern (0,2-4,7- · last · all)", &t->index_pattern, NULL, ripping_changed, c));
  ADD (g, bro_combo_row ("Index numbers refer to", index_base_labels, (int *) &t->index_base, ripping_changed, c));

  g = group (page, "Filters", "0 or empty means no limit. Patterns are regular expressions matched against the title name, comment, source file "
                              "(e.g. 00800.mpls), output file name, segment map and “#&lt;source id&gt;”.");
  ADD (g, bro_duration_row ("Minimum duration (h:mm:ss)", &t->min_duration_seconds, ripping_changed, c));
  ADD (g, bro_duration_row ("Maximum duration (h:mm:ss)", &t->max_duration_seconds, ripping_changed, c));
  ADD (g, bro_spin_row ("Minimum chapters", NULL, &t->min_chapters, 0, 999, ripping_changed, c));
  ADD (g, bro_spin_row ("Maximum chapters", NULL, &t->max_chapters, 0, 999, ripping_changed, c));
  ADD (g, bro_spin_row ("Minimum size", "MB", &t->min_size_mb, 0, 1000000, ripping_changed, c));
  ADD (g, bro_spin_row ("Maximum size", "MB", &t->max_size_mb, 0, 1000000, ripping_changed, c));
  ADD (g, bro_entry_row ("Include if matching", &t->include_pattern, NULL, ripping_changed, c));
  ADD (g, bro_entry_row ("Exclude if matching", &t->exclude_pattern, NULL, ripping_changed, c));
  ADD (g, bro_switch_row ("Skip duplicate titles", "Same segments and length", &t->skip_duplicates, ripping_changed, c));
  ADD (g, bro_switch_row ("Skip alternate angles", NULL, &t->skip_alternate_angles, ripping_changed, c));
  ADD (g, bro_spin_row ("At most", "titles (0 = no limit)", &t->max_titles, 0, 999, ripping_changed, c));

  c->preview = group (page, "Preview", NULL);
  c->preview_rows = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (c->preview_rows), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (c->preview_rows, "boxed-list");
  ADD (c->preview, c->preview_rows);
  refresh_preview (c);

  g = group (page, "makemkvcon options", NULL);
  ADD (g, bro_optional_spin_row ("Minimum title length (--minlength)", "seconds; 0 = use the MakeMKV setting", &d->rip.min_length_seconds, 36000, ripping_changed, c));
  ADD (g, bro_optional_spin_row ("Read cache (--cache)", "MB; 0 = MakeMKV default", &d->rip.cache_mb, 65536, ripping_changed, c));
  c->direct_io_choice = d->rip.direct_io < 0 ? 0 : d->rip.direct_io ? 2 : 1;
  ADD (g, bro_combo_row ("Direct disc access (--directio)", directio_labels, &c->direct_io_choice, ripping_changed, c));
  ADD (g, bro_entry_row ("Extra makemkvcon switches (advanced)", &d->rip.extra_arguments, NULL, ripping_changed, c));
  ADD (g, bro_switch_row ("Save disc information (disc-info.json)", NULL, &d->rip.write_disc_info_json, ripping_changed, c));
  return page;
}

/* ---- output ---- */

static void
on_standard_naming (GtkButton *b, Ctx *c)
{
  gtk_editable_set_text (GTK_EDITABLE (c->folder_row), BRO_DEFAULT_FOLDER_TEMPLATE);
  gtk_editable_set_text (GTK_EDITABLE (c->file_row), BRO_DEFAULT_FILE_TEMPLATE);
}

static AdwPreferencesPage *
output_page (Ctx *c)
{
  AdwPreferencesPage *page = page_new ("Output", "folder-symbolic");
  BroDriveConfig *d = c->config;
  AdwPreferencesGroup *g = group (page, "Location", "Leave the output folder empty to use the global default from Preferences.");
  GString *tokens = g_string_new (NULL);
  ADD (g, bro_path_row ("Output folder", &d->output.root_override, TRUE, ctx_changed, c));
  c->folder_row = bro_entry_row ("Folder name template", &d->output.folder_template, NULL, ctx_changed, c);
  ADD (g, c->folder_row);
  ADD (g, bro_combo_row ("If the folder already exists", conflict_labels, (int *) &d->output.conflict_policy, ctx_changed, c));
  ADD (g, bro_combo_row ("Name output for", layout_labels, (int *) &d->output.layout, ctx_changed, c));
  {
    GtkWidget *note = bro_caption ("Plex / Jellyfin / Emby: Movies/Name (Year)/Name (Year).mkv and TV Shows/Name (Year)/Season 02/"
                                   "Name (Year) - S02E05.mkv, other titles in Other/, backups in Backup/ (ignored by the server). "
                                   "The templates below are not used then; set up online lookup in Preferences for the year.");
    gtk_widget_set_margin_top (note, 6);
    ADD (g, note);
  }
  g = group (page, "File names", "Used for MKV files and backups. Standard naming: {name} - {episode} - {discLabel} - {rip} - {track} - "
                                 "{format}, leaving out parts that don't apply. Format codes: DVD, BR (Blu-ray), 4K (Ultra HD Blu-ray); "
                                 "DVDe, BRe, 4Ke for backups that are not decrypted. Leave empty to keep MakeMKV's own file names.");
  c->file_row = bro_entry_row ("Name files and backups", &d->output.file_name_template, NULL, ctx_changed, c);
  ADD (g, c->file_row);
  {
    GtkWidget *std = gtk_button_new_with_label ("Use the Standard Naming");
    gtk_widget_add_css_class (std, "flat");
    g_signal_connect (std, "clicked", G_CALLBACK (on_standard_naming), c);
    adw_preferences_group_set_header_suffix (g, std);
  }
  ADD (g, bro_entry_row ("Backup subfolder (backup + MKV mode)", &d->output.backup_subfolder, NULL, ctx_changed, c));
  for (int i = 0; bro_file_tokens[i].token; i++)
    g_string_append_printf (tokens, "{%s} — %s\n", bro_file_tokens[i].token, bro_file_tokens[i].help);
  g_string_append (tokens, "{n:3} — zero-pad a number to 3 digits\n{token?text} — insert text only when token is not empty");
  g = group (page, "Tokens", tokens->str);
  g_string_free (tokens, TRUE);
  g = group (page, "Archiving", "Checksums of every file (including backup folders) are saved in the output folder in the standard format; "
                                "check a copy later with “sha256sum -c SHA256SUMS”. The archive record describes the disc, titles, "
                                "episodes and files with their sizes and hashes. Files only reach the output folder when the job "
                                "succeeded; otherwise they are kept in a folder marked [INCOMPLETE] or [READ ERRORS].");
  ADD (g, bro_switch_row ("Write SHA-256 checksums (SHA256SUMS)", NULL, &d->archive.checksums, ctx_changed, c));
  ADD (g, bro_switch_row ("Write an archive record (bromelia.json) and the job log", NULL, &d->archive.archive_record, ctx_changed, c));
  ADD (g, bro_switch_row ("Check every rip against the disc listing",
                          "Compares each MKV's length and tracks with the disc listing (needs mkvmerge) and each backup's structure",
                          &d->archive.verify_rips, ctx_changed, c));
  return page;
}

/* ---- profile ---- */

static void
preset_chosen (GtkButton *b, GtkWidget *row)
{
  gtk_editable_set_text (GTK_EDITABLE (row), g_object_get_data (G_OBJECT (b), "rule"));
  gtk_widget_activate_action (GTK_WIDGET (b), "popover.close", NULL);
}

static GtkWidget *
bro_selection_rule_row (const char *title, char **field, BroChangedFunc changed, gpointer data)
{
  GtkWidget *row = bro_entry_row (title, field, NULL, changed, data);
  GtkWidget *menu = gtk_menu_button_new ();
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  BroCatalog *cat = bro_catalog_get ();
  for (guint i = 0; i < cat->preset_names->len; i++)
    {
      GtkWidget *b = gtk_button_new_with_label (cat->preset_names->pdata[i]);
      gtk_widget_add_css_class (b, "flat");
      g_object_set_data (G_OBJECT (b), "rule", cat->preset_rules->pdata[i]);
      g_signal_connect (b, "clicked", G_CALLBACK (preset_chosen), row);
      gtk_box_append (GTK_BOX (box), b);
    }
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu), pop);
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu), "view-list-symbolic");
  gtk_widget_set_tooltip_text (menu, "Selection rule presets");
  gtk_widget_set_valign (menu, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (menu, "flat");
  adw_entry_row_add_suffix (ADW_ENTRY_ROW (row), menu);
  return row;
}

static char *
selection_help (void)
{
  BroCatalog *cat = bro_catalog_get ();
  GString *s = g_string_new ("Comma separated ‘action:condition’ items applied in order: +sel / -sel select or deselect, "
                             "+N / -N / =N change a track's weight. Conditions combine tokens with | (or), &amp; (and), ! (not) "
                             "and parentheses. Tokens: ");
  for (guint i = 0; i < cat->token_names->len; i++)
    g_string_append_printf (s, "%s%s", i ? ", " : "", (char *) cat->token_names->pdata[i]);
  g_string_append (s, ". Empty = MakeMKV's default rule.");
  return g_string_free (s, FALSE);
}

static void
export_to (const char *path, gpointer data)
{
  BroGeneratedProfile *p = data;
  g_autofree char *xml = bro_profile_build (p, NULL);
  g_file_set_contents (path, xml, -1, NULL);
}

static void
on_export_profile (GtkButton *b, Ctx *c)
{
  g_autofree char *name = g_strdup_printf ("%s.mmcp.xml", c->config->profile.generated.name);
  bro_save_path (GTK_WIDGET (b), "Export profile", name, export_to, &c->config->profile.generated);
}

static AdwPreferencesPage *
profile_page (Ctx *c)
{
  AdwPreferencesPage *page = page_new ("Profile", "document-properties-symbolic");
  BroProfileConfig *p = &c->config->profile;
  g_autofree char *help = selection_help ();
  AdwPreferencesGroup *g = group (page, "Profile", "A MakeMKV profile controls which tracks are selected by default, MKV flags and "
                                                    "audio conversion. It is passed to makemkvcon with --profile.");
  GtkWidget *export_row = adw_action_row_new ();
  GtkWidget *export_btn = gtk_button_new_with_label ("Export…");
  ADD (g, bro_combo_row ("Profile", profile_labels, (int *) &p->mode, ctx_changed, c));
  ADD (g, bro_path_row ("Custom profile file", &p->custom_path, FALSE, ctx_changed, c));
  g = group (page, "Bromelia profile — track selection", help);
  ADD (g, bro_entry_row ("Profile name", &p->generated.name, NULL, ctx_changed, c));
  ADD (g, bro_selection_rule_row ("Selection rule", &p->generated.selection_rule, ctx_changed, c));
  g = group (page, "MKV flags", NULL);
  ADD (g, bro_switch_row ("Mark the first audio track as default", NULL, &p->generated.set_first_audio_default, ctx_changed, c));
  ADD (g, bro_switch_row ("Mark the first subtitle track as default", NULL, &p->generated.set_first_subtitle_default, ctx_changed, c));
  ADD (g, bro_switch_row ("Mark the first forced subtitle track as default", NULL, &p->generated.set_first_forced_subtitle_default, ctx_changed, c));
  ADD (g, bro_switch_row ("Ignore the disc's forced subtitle flag", NULL, &p->generated.ignore_forced_subtitles_flag, ctx_changed, c));
  ADD (g, bro_switch_row ("Use ISO 639-2/T language codes", "deu instead of ger", &p->generated.use_iso639_2t, ctx_changed, c));
  ADD (g, bro_switch_row ("Insert chapter 0 when missing", NULL, &p->generated.insert_first_chapter00, ctx_changed, c));
  g = group (page, "Audio conversion", NULL);
  ADD (g, bro_combo_row ("Stereo / mono LPCM", lpcm_labels, (int *) &p->generated.lpcm_stereo, ctx_changed, c));
  ADD (g, bro_combo_row ("Multichannel LPCM", lpcm_labels, (int *) &p->generated.lpcm_multichannel, ctx_changed, c));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (export_row), "Export the Bromelia profile as .mmcp.xml");
  gtk_widget_set_valign (export_btn, GTK_ALIGN_CENTER);
  g_signal_connect (export_btn, "clicked", G_CALLBACK (on_export_profile), c);
  adw_action_row_add_suffix (ADW_ACTION_ROW (export_row), export_btn);
  ADD (g, export_row);
  return page;
}

/* ---- post-processing ---- */

static void rebuild_post (Ctx *c);

static void
post_move (Ctx *c, BroPostStep *s, int offset)
{
  guint i;
  int k;
  if (!g_ptr_array_find (c->steps, s, &i))
    return;
  k = (int) i + offset;
  if (k < 0 || k >= (int) c->steps->len)
    return;
  c->steps->pdata[i] = c->steps->pdata[k];
  c->steps->pdata[k] = s;
  ctx_changed (c);
  rebuild_post (c);
}

static void on_post_up (GtkButton *b, Ctx *c) { post_move (c, g_object_get_data (G_OBJECT (b), "step"), -1); }
static void on_post_down (GtkButton *b, Ctx *c) { post_move (c, g_object_get_data (G_OBJECT (b), "step"), 1); }

static void
on_post_remove (GtkButton *b, Ctx *c)
{
  g_ptr_array_remove (c->steps, g_object_get_data (G_OBJECT (b), "step"));
  ctx_changed (c);
  rebuild_post (c);
}

static void
env_changed (GtkTextBuffer *buf, Ctx *c)
{
  BroPostStep *s = g_object_get_data (G_OBJECT (buf), "step");
  GtkTextIter a, b;
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  gtk_text_buffer_get_bounds (buf, &a, &b);
  text = gtk_text_buffer_get_text (buf, &a, &b, FALSE);
  lines = g_strsplit (text, "\n", -1);
  g_hash_table_remove_all (s->environment);
  for (int i = 0; lines[i]; i++)
    {
      char *eq = strchr (lines[i], '=');
      if (!eq || eq == lines[i])
        continue;
      *eq = '\0';
      g_hash_table_insert (s->environment, g_strstrip (g_strdup (lines[i])), g_strdup (eq + 1));
    }
  ctx_changed (c);
}

typedef struct {
  BroPostStep *step;
  GString *out;
  GtkWidget *anchor;
} TestRun;

static void
test_line (const char *line, gpointer data)
{
  g_string_append_printf (((TestRun *) data)->out, "%s\n", line);
}

static void
test_thread (GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
  TestRun *t = data;
  g_autoptr (GHashTable) values = bro_template_values_new ();
  g_autofree char *tmp = g_build_filename (g_get_tmp_dir (), "bromelia-test", NULL);
  g_autoptr (GPtrArray) argv = NULL;
  g_auto (GStrv) envp = g_get_environ ();
  int status = -1;
  gboolean timed_out = FALSE;
  g_autoptr (GError) error = NULL;
  GHashTableIter it;
  gpointer k, v;
  g_mkdir_with_parents (tmp, 0755);
  bro_template_values_set (values, "disc", "SAMPLE_DISC");
  bro_template_values_set (values, "volume", "SAMPLE_DISC");
  bro_template_values_set (values, "type", "bd");
  bro_template_values_set (values, "drive", "Test");
  bro_template_values_set (values, "job", "test0000");
  bro_template_values_set (values, "outputDir", tmp);
  bro_template_values_set (values, "status", "success");
  bro_template_values_set (values, "manifest", "");
  bro_template_values_set (values, "file", "");
  bro_template_values_set (values, "files", "");
  bro_template_values_set (values, "device", "");
  bro_template_values_set (values, "checksums", "");
  {
    BroIdentity *id = bro_identity_resolve (NULL, "SAMPLE_DISC", -1, BRO_FORMAT_BLURAY, FALSE, "Sample Disc", BRO_KIND_MOVIE, 0);
    bro_identity_template_values (id, values, "Rip");
    bro_identity_free (id);
  }
  envp = g_environ_setenv (envp, "BROMELIA_NAME", "Sample Disc", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_KIND", "movie", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_FORMAT", "BR", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_ENCRYPTED", "0", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_STATUS", "success", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_DISC_NAME", "SAMPLE_DISC", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_OUTPUT_DIR", tmp, TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_FILE_COUNT", "0", TRUE);
  envp = g_environ_setenv (envp, "BROMELIA_MODE", "mkv", TRUE);
  g_hash_table_iter_init (&it, t->step->environment);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      g_autofree char *rv = bro_template_render (v, values, FALSE);
      envp = g_environ_setenv (envp, k, rv, TRUE);
    }
  argv = bro_post_step_argv (t->step, values, NULL);
  g_ptr_array_add (argv, NULL);
  {
    g_autofree char *cmd = bro_command_line ((const char *const *) argv->pdata);
    g_string_append_printf (t->out, "$ %s\n", cmd);
  }
  if (!bro_process_run ((const char *const *) argv->pdata, (const char *const *) envp, tmp, t->step->timeout_seconds ? t->step->timeout_seconds : 60,
                        NULL, test_line, t, &status, NULL, &timed_out, &error))
    g_string_append_printf (t->out, "Could not start: %s\n", error->message);
  else
    g_string_append_printf (t->out, timed_out ? "Timed out\n" : "Exit status %d\n", status);
  g_task_return_boolean (task, TRUE);
}

static void
test_run_free (TestRun *t)
{
  bro_post_step_free (t->step);
  g_string_free (t->out, TRUE);
  g_object_unref (t->anchor);
  g_free (t);
}

static void
test_done (GObject *src, GAsyncResult *res, gpointer data)
{
  TestRun *t = g_task_get_task_data (G_TASK (res));
  AdwDialog *d = adw_alert_dialog_new ("Test run", NULL);
  GtkWidget *view = gtk_text_view_new ();
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), t->out->str, -1);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), view);
  gtk_widget_set_size_request (scroll, 480, 220);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), scroll);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "close", "Close");
  adw_dialog_present (d, t->anchor);
}

static void
on_post_test (GtkButton *b, Ctx *c)
{
  BroPostStep *s = g_object_get_data (G_OBJECT (b), "step");
  TestRun *t = g_new0 (TestRun, 1);
  GTask *task;
  t->step = bro_post_step_copy (s);
  t->step->run_on = BRO_RUN_ALWAYS;
  t->step->enabled = TRUE;
  t->out = g_string_new (NULL);
  t->anchor = g_object_ref (GTK_WIDGET (b));
  task = g_task_new (NULL, NULL, test_done, NULL);
  g_task_set_task_data (task, t, (GDestroyNotify) test_run_free);
  g_task_run_in_thread (task, test_thread);
  g_object_unref (task);
}

static GtkWidget *
step_button (const char *icon, const char *tip, BroPostStep *s, GCallback cb, Ctx *c)
{
  GtkWidget *b = bro_icon_button (icon, tip);
  g_object_set_data (G_OBJECT (b), "step", s);
  g_signal_connect (b, "clicked", cb, c);
  return b;
}

static void
post_step_changed (gpointer data)
{
  ctx_changed (data);
}

static char *
step_summary (BroPostStep *s)
{
  GString *out = g_string_new (bro_run_condition_label (s->run_on));
  if (s->per_file)
    g_string_append (out, " · per file");
  if (s->match_name && *s->match_name)
    g_string_append_printf (out, " · “%s”", s->match_name);
  for (guint i = 0; i < s->match_formats->len; i++)
    g_string_append_printf (out, "%s%s", i ? ", " : " · ", (char *) s->match_formats->pdata[i]);
  return g_string_free (out, FALSE);
}

static void
on_formats_changed (GtkEditable *e, Ctx *c)
{
  BroPostStep *s = g_object_get_data (G_OBJECT (e), "step");
  g_auto (GStrv) parts = g_strsplit_set (gtk_editable_get_text (e), ", ", -1);
  g_ptr_array_set_size (s->match_formats, 0);
  for (int i = 0; parts[i]; i++)
    if (*parts[i])
      g_ptr_array_add (s->match_formats, g_strdup (parts[i]));
  ctx_changed (c);
}

static void
on_match_name_changed (GtkEditable *e, GtkWidget *row)
{
  g_autofree char *err = bro_plugin_validate (gtk_editable_get_text (e));
  if (err)
    gtk_widget_add_css_class (row, "error");
  else
    gtk_widget_remove_css_class (row, "error");
}

static void
rebuild_post (Ctx *c)
{
  for (guint i = 0; c->post_rows && i < c->post_rows->len; i++)
    adw_preferences_group_remove (c->post_group, c->post_rows->pdata[i]);
  if (c->post_rows)
    g_ptr_array_unref (c->post_rows);
  c->post_rows = g_ptr_array_new ();
  for (guint i = 0; i < c->steps->len; i++)
    {
      BroPostStep *s = c->steps->pdata[i];
      GtkWidget *exp = adw_expander_row_new ();
      GtkWidget *env_row = gtk_list_box_row_new ();
      GtkWidget *env_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      GtkWidget *env_view = gtk_text_view_new ();
      GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (env_view));
      GString *env_text = g_string_new (NULL);
      g_autoptr (GList) keys = g_list_sort (g_hash_table_get_keys (s->environment), (GCompareFunc) g_strcmp0);
      g_autofree char *name = g_markup_escape_text (s->name, -1);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (exp), name);
      {
        g_autofree char *summary = step_summary (s);
        g_autofree char *escaped = g_markup_escape_text (summary, -1);
        adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (exp), escaped);
      }
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), step_button ("media-playback-start-symbolic", "Run with sample values", s, G_CALLBACK (on_post_test), c));
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), step_button ("go-up-symbolic", "Move up", s, G_CALLBACK (on_post_up), c));
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), step_button ("go-down-symbolic", "Move down", s, G_CALLBACK (on_post_down), c));
      adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), step_button ("user-trash-symbolic", "Remove step", s, G_CALLBACK (on_post_remove), c));

      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_entry_row ("Name", &s->name, NULL, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_switch_row ("Enabled", NULL, &s->enabled, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_combo_row ("Run", run_labels, (int *) &s->run_on, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_switch_row ("Run once for every produced file", NULL, &s->per_file, post_step_changed, c));
      {
        GtkWidget *name_row = bro_entry_row ("Only for names or disc labels matching (regular expression)", &s->match_name,
                                             "e.g. ^One Piece$ for a show, or S2_P7 for one disc; empty = every disc", post_step_changed, c);
        GtkWidget *formats = adw_entry_row_new ();
        GString *f = g_string_new (NULL);
        for (guint k = 0; k < s->match_formats->len; k++)
          g_string_append_printf (f, "%s%s", k ? ", " : "", (char *) s->match_formats->pdata[k]);
        g_signal_connect (name_row, "changed", G_CALLBACK (on_match_name_changed), name_row);
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (formats), "Only for formats (DVD, DVDe, BR, BRe, 4K, 4Ke; BR* = BR and BRe)");
        gtk_widget_set_tooltip_text (formats, "Comma separated; empty = every format. The e codes are backups that are not decrypted.");
        gtk_editable_set_text (GTK_EDITABLE (formats), f->str);
        g_string_free (f, TRUE);
        g_object_set_data (G_OBJECT (formats), "step", s);
        g_signal_connect (formats, "changed", G_CALLBACK (on_formats_changed), c);
        adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), name_row);
        adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), formats);
      }
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_path_row ("Program or script", &s->executable, FALSE, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_entry_row ("Interpreter (optional, e.g. /usr/bin/python3)", &s->interpreter, NULL, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_entry_row ("Arguments ({tokens} allowed)", &s->arguments, NULL, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_entry_row ("Working folder (default: output folder)", &s->working_directory, NULL, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_spin_row ("Time limit", "seconds (0 = none)", &s->timeout_seconds, 0, 86400, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_switch_row ("Mark the job as failed if this step fails", NULL, &s->fail_job_on_error, post_step_changed, c));
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), bro_switch_row ("Run in the background",
                                                                       "After the job, once the disc is out (encoding, uploads); can't fail the job",
                                                                       &s->background, post_step_changed, c));

      for (GList *l = keys; l; l = l->next)
        g_string_append_printf (env_text, "%s=%s\n", (char *) l->data, (char *) g_hash_table_lookup (s->environment, l->data));
      gtk_text_buffer_set_text (buf, env_text->str, -1);
      g_string_free (env_text, TRUE);
      g_object_set_data (G_OBJECT (buf), "step", s);
      g_signal_connect (buf, "changed", G_CALLBACK (env_changed), c);
      gtk_text_view_set_monospace (GTK_TEXT_VIEW (env_view), TRUE);
      gtk_widget_set_size_request (env_view, -1, 60);
      gtk_box_append (GTK_BOX (env_box), bro_caption ("Environment variables, one NAME=value per line ({tokens} allowed)"));
      gtk_box_append (GTK_BOX (env_box), env_view);
      gtk_widget_set_margin_start (env_box, 12);
      gtk_widget_set_margin_end (env_box, 12);
      gtk_widget_set_margin_top (env_box, 8);
      gtk_widget_set_margin_bottom (env_box, 8);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (env_row), env_box);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (env_row), FALSE);
      adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), env_row);

      ADD (c->post_group, exp);
      g_ptr_array_add (c->post_rows, exp);
    }
}

static void
add_step (Ctx *c, const char *name, const char *exe, const char *args, BroRunCondition run, gboolean per_file)
{
  BroPostStep *s = bro_post_step_new ();
  g_free (s->name);
  s->name = g_strdup (name);
  g_free (s->executable);
  s->executable = g_strdup (exe);
  g_free (s->arguments);
  s->arguments = g_strdup (args);
  s->run_on = run;
  s->per_file = per_file;
  g_ptr_array_add (c->steps, s);
  ctx_changed (c);
  rebuild_post (c);
}

static void
on_add_step (GtkButton *b, Ctx *c)
{
  const char *kind = g_object_get_data (G_OBJECT (b), "kind");
  gtk_widget_activate_action (GTK_WIDGET (b), "popover.close", NULL);
  if (g_strcmp0 (kind, "move") == 0)
    add_step (c, "Move to library", "/bin/mv", "-n {files} ~/Videos/Library/", BRO_RUN_SUCCESS, FALSE);
  else if (g_strcmp0 (kind, "handbrake") == 0)
    add_step (c, "Encode with HandBrake", "/usr/bin/HandBrakeCLI", "-i {file} -o \"{outputDir}/{stem}.mp4\" --preset \"Fast 1080p30\"", BRO_RUN_SUCCESS, TRUE);
  else if (g_strcmp0 (kind, "log") == 0)
    add_step (c, "Append to rip log", "/bin/sh", "-c 'echo \"$(date) $BROMELIA_STATUS $BROMELIA_DISC_NAME $BROMELIA_OUTPUT_DIR\" >> ~/rips.log'", BRO_RUN_ALWAYS, FALSE);
  else if (g_strcmp0 (kind, "notify") == 0)
    add_step (c, "Desktop notification", "/usr/bin/notify-send", "Bromelia \"{disc}: {status}\"", BRO_RUN_ALWAYS, FALSE);
  else
    {
      g_autofree char *n = g_strdup_printf ("Step %u", c->steps->len + 1);
      add_step (c, n, "", "{outputDir}", BRO_RUN_SUCCESS, FALSE);
    }
}

static AdwPreferencesPage *
post_page (Ctx *c, const char *title, const char *icon, const char *intro)
{
  AdwPreferencesPage *page = page_new (title, icon);
  AdwPreferencesGroup *g;
  GtkWidget *add = gtk_menu_button_new ();
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GString *help = g_string_new (intro);
  g_string_append (help, "Arguments are split like a shell command line and then {tokens} "
                                "are filled in; a lone {files} expands to one argument per file. Environment: BROMELIA_JOB_ID, "
                                "BROMELIA_STATUS, BROMELIA_MODE, BROMELIA_DRIVE_NAME, BROMELIA_DRIVE_ID, BROMELIA_DEVICE, "
                                "BROMELIA_DISC_NAME, BROMELIA_DISC_TYPE, BROMELIA_OUTPUT_DIR, BROMELIA_FILES, BROMELIA_FILE_COUNT, "
                                "BROMELIA_FILE (per-file), BROMELIA_MANIFEST, BROMELIA_LOG, BROMELIA_SOURCE, BROMELIA_ERROR, BROMELIA_NAME, "
                                "BROMELIA_KIND, BROMELIA_FORMAT, BROMELIA_ENCRYPTED, BROMELIA_SEASON, BROMELIA_DISC_NUMBER, BROMELIA_DISC_SET, "
                                "BROMELIA_CHECKSUMS.\nTokens: ");
  const char *kinds[][2] = { { "empty", "Empty step" }, { "move", "Move files to a library folder" }, { "handbrake", "Encode with HandBrakeCLI" },
                             { "log", "Append to a log file" }, { "notify", "Desktop notification" } };
  for (int i = 0; bro_script_tokens[i].token; i++)
    g_string_append_printf (help, "%s{%s}", i ? ", " : "", bro_script_tokens[i].token);
  g = group (page, c->config ? "Steps" : "Plugins", help->str);
  g_string_free (help, TRUE);
  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    {
      GtkWidget *b = gtk_button_new_with_label (kinds[i][1]);
      gtk_widget_add_css_class (b, "flat");
      g_object_set_data (G_OBJECT (b), "kind", (gpointer) kinds[i][0]);
      g_signal_connect (b, "clicked", G_CALLBACK (on_add_step), c);
      gtk_box_append (GTK_BOX (box), b);
    }
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (add), pop);
  gtk_menu_button_set_label (GTK_MENU_BUTTON (add), "Add Step");
  gtk_widget_add_css_class (add, "flat");
  adw_preferences_group_set_header_suffix (g, add);
  c->post_group = g;
  rebuild_post (c);
  return page;
}

static void
ctx_free (Ctx *c)
{
  if (c->post_rows)
    g_ptr_array_unref (c->post_rows);
  g_free (c);
}

GtkWidget *
bro_drive_config_pages_add (AdwPreferencesDialog *dialog, BroDriveConfig *config, gboolean is_default, BroChangedFunc changed, gpointer data)
{
  Ctx *c = g_new0 (Ctx, 1);
  BroState *st = bro_app_state ();
  c->config = config;
  c->is_default = is_default;
  c->changed = changed;
  c->data = data;
  c->dialog = dialog;
  c->steps = config->post_process;
  adw_preferences_dialog_add (dialog, general_page (c));
  adw_preferences_dialog_add (dialog, ripping_page (c));
  adw_preferences_dialog_add (dialog, output_page (c));
  adw_preferences_dialog_add (dialog, ADW_PREFERENCES_PAGE (bro_catalog_page_new (config->settings, st->config->global_settings, TRUE, changed, data)));
  adw_preferences_dialog_add (dialog, profile_page (c));
  adw_preferences_dialog_add (dialog, post_page (c, "Post-processing", "utilities-terminal-symbolic", "Steps run after each job, in order. "));
  g_object_set_data_full (G_OBJECT (dialog), "bro-ctx", c, (GDestroyNotify) ctx_free);
  return GTK_WIDGET (dialog);
}

GtkWidget *
bro_plugins_page_new (GPtrArray *steps, BroChangedFunc changed, gpointer data)
{
  Ctx *c = g_new0 (Ctx, 1);
  AdwPreferencesPage *page;
  c->steps = steps;
  c->changed = changed;
  c->data = data;
  page = post_page (c, "Plugins", "application-x-addon-symbolic",
                    "Plugins are post-processing steps for every drive, usually limited to a movie or show (by name or disc label) "
                    "and to formats such as DVD or 4Ke — for example a script that archives one series in a particular way. They "
                    "run after the drive's own steps. ");
  g_object_set_data_full (G_OBJECT (page), "bro-ctx", c, (GDestroyNotify) ctx_free);
  return GTK_WIDGET (page);
}

static void
state_changed_cb (gpointer data)
{
  bro_state_config_changed (bro_app_state ());
}

static void
delete_response (AdwAlertDialog *a, const char *response, AdwDialog *dialog)
{
  if (!g_str_equal (response, "delete"))
    return;
  bro_state_remove_drive_config (bro_app_state (), g_object_get_data (G_OBJECT (dialog), "config-id"));
  adw_dialog_force_close (dialog);
}

static void
on_delete_config (GtkButton *b, AdwDialog *dialog)
{
  AdwDialog *a = adw_alert_dialog_new ("Delete this drive configuration?", "The drive will use the default configuration again.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (a), "cancel", "Cancel", "delete", "Delete", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (a), "delete", ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect (a, "response", G_CALLBACK (delete_response), dialog);
  adw_dialog_present (a, GTK_WIDGET (b));
}

static void
preset_name_response (AdwAlertDialog *a, const char *response, GtkWidget *entry)
{
  AdwDialog *dialog = g_object_get_data (G_OBJECT (entry), "dialog");
  BroDriveConfig *cfg;
  if (!g_str_equal (response, "save"))
    return;
  cfg = bro_app_config_drive_by_id (bro_app_state ()->config, g_object_get_data (G_OBJECT (dialog), "config-id"));
  if (cfg)
    bro_state_save_preset (bro_app_state (), gtk_editable_get_text (GTK_EDITABLE (entry)), cfg);
}

static void
on_save_preset (GtkButton *b, AdwDialog *dialog)
{
  BroDriveConfig *cfg = bro_app_config_drive_by_id (bro_app_state ()->config, g_object_get_data (G_OBJECT (dialog), "config-id"));
  AdwDialog *a = adw_alert_dialog_new ("Save as preset", "Presets store everything except the drive's name and identification.");
  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), cfg ? cfg->name : "Preset");
  g_object_set_data (G_OBJECT (entry), "dialog", dialog);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (a), entry);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (a), "cancel", "Cancel", "save", "Save", NULL);
  g_signal_connect (a, "response", G_CALLBACK (preset_name_response), entry);
  adw_dialog_present (a, GTK_WIDGET (b));
}

static void
on_apply_preset (GtkButton *b, AdwDialog *dialog)
{
  BroState *st = bro_app_state ();
  BroPreset *preset = g_object_get_data (G_OBJECT (b), "preset");
  const char *id = g_object_get_data (G_OBJECT (dialog), "config-id");
  GtkWidget *anchor = GTK_WIDGET (adw_dialog_get_child (dialog));
  for (guint i = 0; i < st->config->drives->len; i++)
    {
      BroDriveConfig *cur = st->config->drives->pdata[i];
      if (g_str_equal (cur->id, id))
        {
          BroDriveConfig *replacement = bro_drive_config_apply_body (cur, preset->config);
          st->config->drives->pdata[i] = replacement;
          bro_drive_config_free (cur);
          bro_state_config_changed (st);
          break;
        }
    }
  /* The open pages are bound to the old object: reopen the dialog. */
  {
    GtkWidget *parent = gtk_widget_get_parent (anchor);
    g_autofree char *cid = g_strdup (id);
    adw_dialog_force_close (dialog);
    bro_config_dialog_present (parent ? parent : anchor, cid);
  }
}

static void
on_archive_everything (GtkButton *b, AdwDialog *dialog)
{
  BroState *st = bro_app_state ();
  const char *id = g_object_get_data (G_OBJECT (dialog), "config-id");
  GtkWidget *anchor = GTK_WIDGET (adw_dialog_get_child (dialog));
  BroDriveConfig *cfg = bro_app_config_drive_by_id (st->config, id);
  if (!cfg)
    return;
  bro_drive_config_apply_archive_everything (cfg);
  bro_state_config_changed (st);
  /* The open pages show the old values: reopen the dialog. */
  {
    GtkWidget *parent = gtk_widget_get_parent (anchor);
    g_autofree char *cid = g_strdup (id);
    adw_dialog_force_close (dialog);
    bro_config_dialog_present (parent ? parent : anchor, cid);
  }
}

void
bro_config_dialog_present (GtkWidget *parent, const char *config_id)
{
  BroState *st = bro_app_state ();
  BroDriveConfig *cfg = bro_app_config_drive_by_id (st->config, config_id);
  gboolean is_default = cfg == st->config->default_drive;
  AdwDialog *dialog;
  AdwPreferencesPage *general;
  AdwPreferencesGroup *g;
  if (!cfg)
    return;
  dialog = adw_preferences_dialog_new ();
  {
    g_autofree char *title = g_strdup_printf ("Configure “%s”", cfg->name);
    adw_dialog_set_title (dialog, title);
  }
  adw_dialog_set_content_width (dialog, 760);
  adw_dialog_set_content_height (dialog, 720);
  g_object_set_data_full (G_OBJECT (dialog), "config-id", g_strdup (config_id), g_free);
  bro_drive_config_pages_add (ADW_PREFERENCES_DIALOG (dialog), cfg, is_default, state_changed_cb, NULL);

  /* Presets and deletion on the first page. */
  general = ADW_PREFERENCES_PAGE (adw_preferences_dialog_get_visible_page (ADW_PREFERENCES_DIALOG (dialog)));
  if (general && !is_default)
    {
      GtkWidget *row = adw_action_row_new ();
      GtkWidget *save = gtk_button_new_with_label ("Save as Preset…");
      GtkWidget *del = gtk_button_new_with_label ("Delete…");
      g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      adw_preferences_group_set_title (g, "Presets and removal");
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Configuration");
      gtk_widget_set_valign (save, GTK_ALIGN_CENTER);
      gtk_widget_set_valign (del, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (del, "destructive-action");
      g_signal_connect (save, "clicked", G_CALLBACK (on_save_preset), dialog);
      g_signal_connect (del, "clicked", G_CALLBACK (on_delete_config), dialog);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), save);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), del);
      adw_preferences_group_add (g, row);
      {
        GtkWidget *erow = adw_action_row_new ();
        GtkWidget *apply = gtk_button_new_with_label ("Apply");
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (erow), "Archive everything");
        adw_action_row_set_subtitle (ADW_ACTION_ROW (erow), "Backup then MKV, every track, all checks on");
        gtk_widget_set_valign (apply, GTK_ALIGN_CENTER);
        g_signal_connect (apply, "clicked", G_CALLBACK (on_archive_everything), dialog);
        adw_action_row_add_suffix (ADW_ACTION_ROW (erow), apply);
        adw_preferences_group_add (g, erow);
      }
      for (guint i = 0; i < st->config->presets->len; i++)
        {
          BroPreset *p = st->config->presets->pdata[i];
          GtkWidget *prow = adw_action_row_new ();
          GtkWidget *apply = gtk_button_new_with_label ("Apply");
          g_autofree char *name = g_markup_escape_text (p->name, -1);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (prow), name);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (prow), "Preset");
          gtk_widget_set_valign (apply, GTK_ALIGN_CENTER);
          g_object_set_data (G_OBJECT (apply), "preset", p);
          g_signal_connect (apply, "clicked", G_CALLBACK (on_apply_preset), dialog);
          adw_action_row_add_suffix (ADW_ACTION_ROW (prow), apply);
          adw_preferences_group_add (g, prow);
        }
      adw_preferences_page_add (general, g);
    }
  if (g_getenv ("BROMELIA_SNAPSHOT_PAGE")) /* developer aid, see main.c */
    adw_preferences_dialog_set_visible_page_name (ADW_PREFERENCES_DIALOG (dialog), g_getenv ("BROMELIA_SNAPSHOT_PAGE"));
  adw_dialog_present (dialog, parent);
}

/* ---- settings catalog ---- */

typedef struct {
  GHashTable *settings;
  GHashTable *global;
  char *key;
  const BroCatalogSetting *setting;
  GtkWidget *editor;
  GtkWidget *inherited;
  BroChangedFunc changed;
  gpointer data;
  gboolean drive_mode;
} SettingRow;

static void
setting_row_free (SettingRow *r)
{
  g_free (r->key);
  g_free (r);
}

static const char *
current_value (SettingRow *r)
{
  const char *v = g_hash_table_lookup (r->settings, r->key);
  if (v)
    return v;
  if (r->drive_mode)
    {
      v = g_hash_table_lookup (r->global, r->key);
      if (v)
        return v;
    }
  return r->setting->def ? r->setting->def : "";
}

static void
set_value (SettingRow *r, const char *value)
{
  const char *old = g_hash_table_lookup (r->settings, r->key);
  if (r->drive_mode && !old)
    return; /* not overridden */
  if (g_strcmp0 (old, value) == 0)
    return;
  g_hash_table_replace (r->settings, g_strdup (r->key), g_strdup (value));
  if (r->changed)
    r->changed (r->data);
}

static void on_setting_switch (GObject *sw, GParamSpec *p, SettingRow *r) { set_value (r, gtk_switch_get_active (GTK_SWITCH (sw)) ? "1" : "0"); }
static void on_setting_entry (GtkEditable *e, SettingRow *r) { set_value (r, gtk_editable_get_text (e)); }

static void
on_setting_choice (GObject *dd, GParamSpec *p, SettingRow *r)
{
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
  if (sel != GTK_INVALID_LIST_POSITION && r->setting->choice_values && sel < r->setting->choice_values->len)
    set_value (r, r->setting->choice_values->pdata[sel]);
}

static char *
inherited_text (SettingRow *r)
{
  const char *v = g_hash_table_lookup (r->global, r->key);
  if (!v || !*v)
    return g_strdup ("Inherited: MakeMKV default");
  if (g_str_equal (r->setting->type, "bool"))
    return g_strdup_printf ("Inherited: %s", g_str_equal (v, "1") ? "On" : "Off");
  if (r->setting->choice_values)
    for (guint i = 0; i < r->setting->choice_values->len; i++)
      if (g_str_equal (r->setting->choice_values->pdata[i], v))
        return g_strdup_printf ("Inherited: %s", (char *) r->setting->choice_labels->pdata[i]);
  return g_strdup_printf ("Inherited: %s", v);
}

static void
on_override (GtkCheckButton *cb, SettingRow *r)
{
  gboolean on = gtk_check_button_get_active (cb);
  if (on)
    {
      const char *g = g_hash_table_lookup (r->global, r->key);
      g_hash_table_replace (r->settings, g_strdup (r->key), g_strdup (g ? g : (r->setting->def ? r->setting->def : "")));
    }
  else
    g_hash_table_remove (r->settings, r->key);
  gtk_widget_set_sensitive (r->editor, on);
  gtk_widget_set_visible (r->inherited, !on);
  if (r->changed)
    r->changed (r->data);
}

static void
setting_path_picked (const char *path, gpointer data)
{
  gtk_editable_set_text (GTK_EDITABLE (data), path);
}

static void
on_setting_browse (GtkButton *b, GtkWidget *entry)
{
  gboolean folder = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "folder"));
  bro_pick_path (GTK_WIDGET (b), folder, "Choose", setting_path_picked, entry);
}

static GtkWidget *
setting_row (GHashTable *settings, GHashTable *global, const BroCatalogSetting *s, gboolean drive_mode, BroChangedFunc changed, gpointer data)
{
  SettingRow *r = g_new0 (SettingRow, 1);
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *editor;
  g_autofree char *title = g_strdup_printf ("%s%s", s->label, s->advanced ? " (advanced)" : "");
  r->settings = settings;
  r->global = global;
  r->key = g_strdup (s->key);
  r->setting = s;
  r->changed = changed;
  r->data = data;
  r->drive_mode = drive_mode;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (s->help)
    {
      g_autofree char *help = g_markup_escape_text (s->help, -1);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), help);
    }

  if (g_str_equal (s->type, "bool"))
    {
      editor = gtk_switch_new ();
      gtk_switch_set_active (GTK_SWITCH (editor), g_str_equal (current_value (r), "1"));
      g_signal_connect (editor, "notify::active", G_CALLBACK (on_setting_switch), r);
    }
  else if (g_str_equal (s->type, "choice") && s->choice_labels)
    {
      g_autoptr (GtkStringList) labels = gtk_string_list_new (NULL);
      guint sel = 0;
      for (guint i = 0; i < s->choice_labels->len; i++)
        {
          gtk_string_list_append (labels, s->choice_labels->pdata[i]);
          if (g_str_equal (s->choice_values->pdata[i], current_value (r)))
            sel = i;
        }
      editor = gtk_drop_down_new (G_LIST_MODEL (g_object_ref (labels)), NULL);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (editor), sel);
      g_signal_connect (editor, "notify::selected", G_CALLBACK (on_setting_choice), r);
    }
  else
    {
      GtkWidget *entry = gtk_entry_new ();
      gtk_editable_set_text (GTK_EDITABLE (entry), current_value (r));
      gtk_editable_set_width_chars (GTK_EDITABLE (entry), g_str_equal (s->type, "selection") ? 36 : 22);
      if (g_str_equal (s->type, "language"))
        gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "e.g. eng");
      g_signal_connect (entry, "changed", G_CALLBACK (on_setting_entry), r);
      if (g_str_equal (s->type, "file") || g_str_equal (s->type, "directory"))
        {
          GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
          GtkWidget *btn = bro_icon_button ("document-open-symbolic", "Choose…");
          g_object_set_data (G_OBJECT (btn), "folder", GINT_TO_POINTER (g_str_equal (s->type, "directory")));
          g_signal_connect (btn, "clicked", G_CALLBACK (on_setting_browse), entry);
          gtk_box_append (GTK_BOX (box), entry);
          gtk_box_append (GTK_BOX (box), btn);
          editor = box;
        }
      else
        editor = entry;
    }
  gtk_widget_set_valign (editor, GTK_ALIGN_CENTER);
  r->editor = editor;
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), editor);
  r->inherited = gtk_label_new (NULL);
  if (drive_mode)
    {
      GtkWidget *cb = gtk_check_button_new ();
      gboolean overridden = g_hash_table_contains (settings, s->key);
      g_autofree char *inh = inherited_text (r);
      gtk_widget_set_tooltip_text (cb, "Override for this drive");
      gtk_check_button_set_active (GTK_CHECK_BUTTON (cb), overridden);
      gtk_widget_set_valign (cb, GTK_ALIGN_CENTER);
      g_signal_connect (cb, "toggled", G_CALLBACK (on_override), r);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), cb);
      gtk_label_set_text (GTK_LABEL (r->inherited), inh);
      gtk_widget_add_css_class (r->inherited, "caption");
      gtk_widget_add_css_class (r->inherited, "dim-label");
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), r->inherited);
      gtk_widget_set_sensitive (editor, overridden);
      gtk_widget_set_visible (r->inherited, !overridden);
    }
  g_object_set_data_full (G_OBJECT (row), "setting-row", r, (GDestroyNotify) setting_row_free);
  return row;
}

typedef struct {
  GHashTable *settings;
  BroChangedFunc changed;
  gpointer data;
  AdwPreferencesGroup *group;
  GPtrArray *rows;
  GtkWidget *key_entry, *value_entry;
} Extra;

static void extra_rebuild (Extra *x);

static void
extra_value_changed (GtkEditable *e, Extra *x)
{
  const char *key = g_object_get_data (G_OBJECT (e), "key");
  g_hash_table_replace (x->settings, g_strdup (key), g_strdup (gtk_editable_get_text (e)));
  if (x->changed) x->changed (x->data);
}

static void
extra_remove (GtkButton *b, Extra *x)
{
  g_hash_table_remove (x->settings, g_object_get_data (G_OBJECT (b), "key"));
  if (x->changed) x->changed (x->data);
  extra_rebuild (x);
}

static void
extra_add (GtkButton *b, Extra *x)
{
  g_autofree char *key = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (x->key_entry))));
  if (!*key)
    return;
  g_hash_table_replace (x->settings, g_strdup (key), g_strdup (gtk_editable_get_text (GTK_EDITABLE (x->value_entry))));
  gtk_editable_set_text (GTK_EDITABLE (x->key_entry), "");
  gtk_editable_set_text (GTK_EDITABLE (x->value_entry), "");
  if (x->changed) x->changed (x->data);
  extra_rebuild (x);
}

static void
extra_rebuild (Extra *x)
{
  BroCatalog *cat = bro_catalog_get ();
  g_autoptr (GList) keys = g_list_sort (g_hash_table_get_keys (x->settings), (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < x->rows->len; i++)
    adw_preferences_group_remove (x->group, x->rows->pdata[i]);
  g_ptr_array_set_size (x->rows, 0);
  for (GList *l = keys; l; l = l->next)
    {
      GtkWidget *row, *del;
      if (bro_catalog_has_key (cat, l->data) || g_str_equal (l->data, "app_Key"))
        continue;
      row = adw_entry_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), l->data);
      gtk_editable_set_text (GTK_EDITABLE (row), g_hash_table_lookup (x->settings, l->data));
      g_object_set_data_full (G_OBJECT (row), "key", g_strdup (l->data), g_free);
      g_signal_connect (row, "changed", G_CALLBACK (extra_value_changed), x);
      del = bro_icon_button ("user-trash-symbolic", "Remove");
      g_object_set_data_full (G_OBJECT (del), "key", g_strdup (l->data), g_free);
      g_signal_connect (del, "clicked", G_CALLBACK (extra_remove), x);
      adw_entry_row_add_suffix (ADW_ENTRY_ROW (row), del);
      adw_preferences_group_add (x->group, row);
      g_ptr_array_add (x->rows, row);
    }
}

static void
extra_free (Extra *x)
{
  g_ptr_array_unref (x->rows);
  g_free (x);
}

GtkWidget *
bro_catalog_page_new (GHashTable *settings, GHashTable *global, gboolean drive_mode, BroChangedFunc changed, gpointer data)
{
  AdwPreferencesPage *page = page_new ("MakeMKV", "preferences-system-symbolic");
  BroCatalog *cat = bro_catalog_get ();
  AdwPreferencesGroup *g = group (page, NULL, drive_mode
                                  ? "Checked settings override the global MakeMKV settings for this drive only. Each drive runs makemkvcon "
                                    "with its own settings.conf, so drives never share these values."
                                  : "These settings apply to every drive unless a drive overrides them. They are written to a private "
                                    "settings.conf for each job; MakeMKV's own settings are not modified.");
  Extra *x = g_new0 (Extra, 1);
  GtkWidget *add_row = adw_action_row_new ();
  GtkWidget *add_btn = bro_icon_button ("list-add-symbolic", "Add setting");
  (void) g;
  for (guint i = 0; i < cat->sections->len; i++)
    {
      BroCatalogSection *sec = cat->sections->pdata[i];
      AdwPreferencesGroup *sg = group (page, sec->title, NULL);
      for (guint j = 0; j < sec->settings->len; j++)
        {
          BroCatalogSetting *s = sec->settings->pdata[j];
          if (s->this_platform)
            adw_preferences_group_add (sg, setting_row (settings, global, s, drive_mode, changed, data));
        }
    }
  x->settings = settings;
  x->changed = changed;
  x->data = data;
  x->rows = g_ptr_array_new ();
  x->group = group (page, "Additional settings", "Any other settings.conf key, written as is.");
  x->key_entry = gtk_entry_new ();
  x->value_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (x->key_entry), "setting_Key");
  gtk_entry_set_placeholder_text (GTK_ENTRY (x->value_entry), "value");
  gtk_widget_set_valign (x->key_entry, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (x->value_entry, GTK_ALIGN_CENTER);
  g_signal_connect (add_btn, "clicked", G_CALLBACK (extra_add), x);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (add_row), "New setting");
  adw_action_row_add_suffix (ADW_ACTION_ROW (add_row), x->key_entry);
  adw_action_row_add_suffix (ADW_ACTION_ROW (add_row), x->value_entry);
  adw_action_row_add_suffix (ADW_ACTION_ROW (add_row), add_btn);
  adw_preferences_group_add (x->group, add_row);
  extra_rebuild (x);
  g_object_set_data_full (G_OBJECT (page), "extra", x, (GDestroyNotify) extra_free);
  return GTK_WIDGET (page);
}
