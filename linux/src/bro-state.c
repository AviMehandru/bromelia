/* bro-state.c — application state, drive scanning, disc sessions, job queue and history. */
#include "bro-state.h"
#include "bro-logic.h"

#include <json-glib/json-glib.h>
#include <string.h>

void
bro_log_entry_free (BroLogEntry *e)
{
  if (!e)
    return;
  g_free (e->text);
  g_free (e);
}

static BroLogEntry *
log_entry_new (BroSeverity sev, const char *text)
{
  BroLogEntry *e = g_new0 (BroLogEntry, 1);
  e->time = g_get_real_time () / G_USEC_PER_SEC;
  e->severity = sev;
  e->text = g_strdup (text);
  return e;
}

char *
bro_data_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "bromelia", NULL);
}

/* ================================ job ================================ */

enum { JOB_CHANGED, JOB_LOG_ADDED, JOB_N_SIGNALS };
static guint job_signals[JOB_N_SIGNALS];

G_DEFINE_FINAL_TYPE (BroJob, bro_job, G_TYPE_OBJECT)

static void
bro_job_finalize (GObject *obj)
{
  BroJob *j = BRO_JOB (obj);
  g_free (j->id);
  g_free (j->lane);
  g_free (j->source_label);
  g_free (j->disc_label);
  bro_source_free (j->source);
  bro_drive_config_free (j->drive);
  if (j->preloaded) bro_disc_info_unref (j->preloaded);
  if (j->manual_titles) g_array_unref (j->manual_titles);
  g_hash_table_unref (j->track_selections);
  g_hash_table_unref (j->name_overrides);
  g_free (j->phase);
  g_free (j->operation);
  g_free (j->total_operation);
  g_free (j->output_dir);
  g_ptr_array_unref (j->files);
  g_free (j->error);
  g_ptr_array_unref (j->log);
  g_ptr_array_unref (j->commands);
  g_clear_object (&j->cancellable);
  G_OBJECT_CLASS (bro_job_parent_class)->finalize (obj);
}

static void
bro_job_class_init (BroJobClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_job_finalize;
  job_signals[JOB_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  job_signals[JOB_LOG_ADDED] = g_signal_new ("log-added", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                             G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
bro_job_init (BroJob *j)
{
  j->id = g_uuid_string_random ();
  j->track_selections = bro_track_selections_new ();
  j->name_overrides = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  j->phase = g_strdup ("Queued");
  j->operation = g_strdup ("");
  j->total_operation = g_strdup ("");
  j->step_count = 1;
  j->files = g_ptr_array_new_with_free_func (g_free);
  j->log = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_entry_free);
  j->commands = g_ptr_array_new_with_free_func (g_free);
}

static BroJob *
job_new (BroSource *source, const BroDriveConfig *drive, const char *lane, const char *label, const char *disc, BroRipMode mode)
{
  BroJob *j = g_object_new (BRO_TYPE_JOB, NULL);
  j->source = source;
  j->drive = bro_drive_config_copy (drive);
  j->lane = g_strdup (lane);
  j->source_label = g_strdup (label);
  j->disc_label = g_strdup (disc ? disc : "");
  j->mode = mode;
  return j;
}

static void
job_changed (BroJob *j)
{
  g_signal_emit (j, job_signals[JOB_CHANGED], 0);
}

static void
job_log (BroJob *j, BroSeverity sev, const char *text)
{
  BroLogEntry *e = log_entry_new (sev, text);
  g_ptr_array_add (j->log, e);
  if (j->log->len > 22000)
    g_ptr_array_remove_range (j->log, 0, 2000);
  if (sev == BRO_SEV_WARNING) j->warnings++;
  if (sev == BRO_SEV_ERROR) j->errors++;
  g_signal_emit (j, job_signals[JOB_LOG_ADDED], 0, e);
}

double
bro_job_overall (BroJob *j)
{
  double v;
  if (j->state == BRO_JOB_SUCCEEDED)
    return 1;
  v = (j->step_index + CLAMP (j->total, 0, 1)) / MAX (j->step_count, 1);
  return CLAMP (v, 0, 1);
}

char *
bro_job_title (BroJob *j)
{
  return g_strdup_printf ("%s — %s", j->disc_label && *j->disc_label ? j->disc_label : "Disc", j->source_label);
}

gint64
bro_job_elapsed (BroJob *j)
{
  if (!j->started_at)
    return 0;
  return (j->finished_at ? j->finished_at : g_get_real_time () / G_USEC_PER_SEC) - j->started_at;
}

gint64
bro_job_remaining (BroJob *j)
{
  double p = bro_job_overall (j);
  gint64 e = bro_job_elapsed (j);
  if (j->state != BRO_JOB_RUNNING || p < 0.02 || e < 10)
    return -1;
  return (gint64) (e / p * (1 - p));
}

char *
bro_job_dir (BroJob *j)
{
  g_autofree char *data = bro_data_dir ();
  return g_build_filename (data, "jobs", j->id, NULL);
}

char *
bro_job_log_path (BroJob *j)
{
  g_autofree char *dir = bro_job_dir (j);
  return g_build_filename (dir, "log.txt", NULL);
}

/* ================================ session ================================ */

enum { SESSION_CHANGED, SESSION_SELECTION_CHANGED, SESSION_LOG_ADDED, SESSION_N_SIGNALS };
static guint session_signals[SESSION_N_SIGNALS];

G_DEFINE_FINAL_TYPE (BroSession, bro_session, G_TYPE_OBJECT)

static void
bro_session_finalize (GObject *obj)
{
  BroSession *s = BRO_SESSION (obj);
  g_free (s->id);
  bro_source_free (s->source);
  g_free (s->config_id);
  if (s->info) bro_disc_info_unref (s->info);
  g_free (s->error);
  g_free (s->operation);
  g_ptr_array_unref (s->log);
  g_hash_table_unref (s->selected);
  g_hash_table_unref (s->track_selections);
  g_hash_table_unref (s->name_overrides);
  g_free (s->output_override);
  g_clear_object (&s->cancellable);
  G_OBJECT_CLASS (bro_session_parent_class)->finalize (obj);
}

static void
bro_session_class_init (BroSessionClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_session_finalize;
  session_signals[SESSION_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  session_signals[SESSION_SELECTION_CHANGED] = g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                                             NULL, NULL, NULL, G_TYPE_NONE, 0);
  session_signals[SESSION_LOG_ADDED] = g_signal_new ("log-added", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                                     G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
bro_session_init (BroSession *s)
{
  s->operation = g_strdup ("");
  s->log = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_entry_free);
  s->selected = bro_int_set_new ();
  s->track_selections = bro_track_selections_new ();
  s->name_overrides = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  s->output_override = g_strdup ("");
}

static BroSession *
session_new (const char *id, BroSource *source, const char *config_id)
{
  BroSession *s = g_object_new (BRO_TYPE_SESSION, NULL);
  s->id = g_strdup (id);
  s->source = source;
  s->config_id = g_strdup (config_id);
  return s;
}

static void
session_changed (BroSession *s)
{
  g_signal_emit (s, session_signals[SESSION_CHANGED], 0);
}

static void
selection_changed (BroSession *s)
{
  g_signal_emit (s, session_signals[SESSION_SELECTION_CHANGED], 0);
}

static void
session_log (BroSession *s, BroSeverity sev, const char *text)
{
  BroLogEntry *e = log_entry_new (sev, text);
  g_ptr_array_add (s->log, e);
  if (s->log->len > 6000)
    g_ptr_array_remove_range (s->log, 0, 1000);
  g_signal_emit (s, session_signals[SESSION_LOG_ADDED], 0, e);
}

void
bro_session_reset (BroSession *s)
{
  if (s->cancellable)
    g_cancellable_cancel (s->cancellable);
  g_clear_object (&s->cancellable);
  g_clear_pointer (&s->info, bro_disc_info_unref);
  s->loading = FALSE;
  g_clear_pointer (&s->error, g_free);
  s->progress = 0;
  g_free (s->operation);
  s->operation = g_strdup ("");
  g_hash_table_remove_all (s->selected);
  g_hash_table_remove_all (s->track_selections);
  g_hash_table_remove_all (s->name_overrides);
  session_changed (s);
  selection_changed (s);
}

void
bro_session_apply_rule (BroSession *s, const BroTitleSelection *rule)
{
  BroTitleSelection r;
  g_autoptr (BroSelectionResult) res = NULL;
  g_autoptr (GArray) sel = NULL;
  if (!s->info)
    return;
  r = *rule;
  if (r.strategy == BRO_STRATEGY_MANUAL)
    r.strategy = BRO_STRATEGY_ALL;
  res = bro_select_titles (s->info, &r);
  sel = bro_selection_result_selected (res);
  g_hash_table_remove_all (s->selected);
  for (guint i = 0; i < sel->len; i++)
    g_hash_table_add (s->selected, GINT_TO_POINTER (g_array_index (sel, int, i)));
  g_hash_table_remove_all (s->track_selections);
  selection_changed (s);
}

void
bro_session_set_selected (BroSession *s, int title, gboolean selected)
{
  if (selected)
    g_hash_table_add (s->selected, GINT_TO_POINTER (title));
  else
    g_hash_table_remove (s->selected, GINT_TO_POINTER (title));
  selection_changed (s);
}

void
bro_session_select_all (BroSession *s, gboolean all)
{
  g_hash_table_remove_all (s->selected);
  if (all && s->info)
    for (guint i = 0; i < s->info->titles->len; i++)
      g_hash_table_add (s->selected, GINT_TO_POINTER (((BroTitle *) s->info->titles->pdata[i])->index));
  selection_changed (s);
}

gboolean
bro_session_has_custom_tracks (BroSession *s, int title)
{
  return g_hash_table_contains (s->track_selections, GINT_TO_POINTER (title));
}

void
bro_session_customize_tracks (BroSession *s, int title)
{
  BroTitle *t = s->info ? bro_disc_info_title (s->info, title) : NULL;
  GHashTable *set;
  if (!t)
    return;
  set = bro_int_set_new ();
  for (guint i = 0; i < t->tracks->len; i++)
    g_hash_table_add (set, GINT_TO_POINTER (((BroTrack *) t->tracks->pdata[i])->index));
  g_hash_table_replace (s->track_selections, GINT_TO_POINTER (title), set);
  selection_changed (s);
}

void
bro_session_reset_tracks (BroSession *s, int title)
{
  g_hash_table_remove (s->track_selections, GINT_TO_POINTER (title));
  selection_changed (s);
}

gboolean
bro_session_track_selected (BroSession *s, int title, int track)
{
  GHashTable *set = g_hash_table_lookup (s->track_selections, GINT_TO_POINTER (title));
  return set && g_hash_table_contains (set, GINT_TO_POINTER (track));
}

void
bro_session_set_track (BroSession *s, int title, int track, gboolean selected)
{
  GHashTable *set = g_hash_table_lookup (s->track_selections, GINT_TO_POINTER (title));
  if (!set)
    return;
  if (selected)
    g_hash_table_add (set, GINT_TO_POINTER (track));
  else
    g_hash_table_remove (set, GINT_TO_POINTER (track));
  selection_changed (s);
}

gint64
bro_session_selected_size (BroSession *s)
{
  gint64 total = 0;
  if (!s->info)
    return 0;
  for (guint i = 0; i < s->info->titles->len; i++)
    {
      BroTitle *t = s->info->titles->pdata[i];
      if (g_hash_table_contains (s->selected, GINT_TO_POINTER (t->index)))
        total += bro_title_size (t);
    }
  return total;
}

/* ================================ state ================================ */

enum { STATE_DRIVES_CHANGED, STATE_JOBS_CHANGED, STATE_HISTORY_CHANGED, STATE_STATUS_CHANGED, STATE_TICK, STATE_N_SIGNALS };
static guint state_signals[STATE_N_SIGNALS];

G_DEFINE_FINAL_TYPE (BroState, bro_state, G_TYPE_OBJECT)

static void
history_record_free (BroHistoryRecord *r)
{
  if (!r)
    return;
  g_free (r->id);
  g_free (r->title);
  g_free (r->drive_name);
  g_free (r->disc_name);
  g_free (r->mode);
  g_free (r->state);
  g_free (r->output_dir);
  g_free (r->error);
  g_free (r->log_path);
  g_ptr_array_unref (r->files);
  g_free (r);
}

void
bro_drive_item_free (BroDriveItem *item)
{
  if (!item)
    return;
  g_free (item->id);
  g_free (item->lane);
  bro_drive_entry_free (item->entry);
  g_free (item);
}

char *
bro_drive_item_name (BroDriveItem *item)
{
  if (item->config)
    return g_strdup (item->config->name);
  return item->entry ? bro_drive_short_model (item->entry->drive_name) : g_strdup ("Drive");
}

static void
bro_state_finalize (GObject *obj)
{
  BroState *self = BRO_STATE (obj);
  g_clear_handle_id (&self->save_source, g_source_remove);
  g_clear_handle_id (&self->tick_source, g_source_remove);
  g_clear_handle_id (&self->poll_source, g_source_remove);
  g_clear_handle_id (&self->rescan_source, g_source_remove);
  g_clear_object (&self->monitor);
  bro_app_config_free (self->config);
  g_free (self->config_path);
  g_ptr_array_unref (self->drives);
  g_hash_table_unref (self->known_states);
  g_hash_table_unref (self->sessions);
  g_ptr_array_unref (self->file_sessions);
  g_ptr_array_unref (self->jobs);
  g_ptr_array_unref (self->history);
  g_free (self->makemkv_version);
  g_free (self->last_error);
  g_ptr_array_unref (self->scan_messages);
  G_OBJECT_CLASS (bro_state_parent_class)->finalize (obj);
}

static void
bro_state_class_init (BroStateClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_state_finalize;
  state_signals[STATE_DRIVES_CHANGED] = g_signal_new ("drives-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  state_signals[STATE_JOBS_CHANGED] = g_signal_new ("jobs-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  state_signals[STATE_HISTORY_CHANGED] = g_signal_new ("history-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  state_signals[STATE_STATUS_CHANGED] = g_signal_new ("status-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  /* Emitted every second while jobs are running or waiting (for progress text in lists). */
  state_signals[STATE_TICK] = g_signal_new ("tick", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
bro_state_init (BroState *self)
{
  self->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_entry_free);
  self->known_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->sessions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->file_sessions = g_ptr_array_new_with_free_func (g_object_unref);
  self->jobs = g_ptr_array_new_with_free_func (g_object_unref);
  self->history = g_ptr_array_new_with_free_func ((GDestroyNotify) history_record_free);
  self->makemkv_version = g_strdup ("");
  self->scan_messages = g_ptr_array_new_with_free_func (g_free);
}

static void emit (BroState *self, guint sig) { g_signal_emit (self, state_signals[sig], 0); }

/* ---- history persistence ---- */

static char *
history_path (void)
{
  g_autofree char *dir = bro_data_dir ();
  return g_build_filename (dir, "history.json", NULL);
}

static void
load_history (BroState *self)
{
  g_autofree char *path = history_path ();
  g_autoptr (JsonParser) p = json_parser_new ();
  JsonArray *arr;
  if (!json_parser_load_from_file (p, path, NULL) || !JSON_NODE_HOLDS_ARRAY (json_parser_get_root (p)))
    return;
  arr = json_node_get_array (json_parser_get_root (p));
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonObject *o = json_array_get_object_element (arr, i);
      BroHistoryRecord *r = g_new0 (BroHistoryRecord, 1);
#define GS(k) (json_object_has_member (o, k) && JSON_NODE_HOLDS_VALUE (json_object_get_member (o, k)) ? g_strdup (json_object_get_string_member (o, k)) : NULL)
      r->id = GS ("id");
      r->title = GS ("title");
      r->drive_name = GS ("driveName");
      r->disc_name = GS ("discName");
      r->mode = GS ("mode");
      r->state = GS ("state");
      r->output_dir = GS ("outputDirectory");
      r->error = GS ("errorMessage");
      r->log_path = GS ("logPath");
#undef GS
      r->started_at = json_object_get_int_member_with_default (o, "startedAt", 0);
      r->finished_at = json_object_get_int_member_with_default (o, "finishedAt", 0);
      r->warnings = json_object_get_int_member_with_default (o, "warnings", 0);
      r->errors = json_object_get_int_member_with_default (o, "errors", 0);
      r->files = g_ptr_array_new_with_free_func (g_free);
      if (json_object_has_member (o, "files"))
        {
          JsonArray *f = json_object_get_array_member (o, "files");
          for (guint k = 0; k < json_array_get_length (f); k++)
            g_ptr_array_add (r->files, g_strdup (json_array_get_string_element (f, k)));
        }
      g_ptr_array_add (self->history, r);
    }
}

static void
save_history (BroState *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL;
  g_autofree char *path = history_path ();
  json_builder_begin_array (b);
  for (guint i = 0; i < self->history->len; i++)
    {
      BroHistoryRecord *r = self->history->pdata[i];
      json_builder_begin_object (b);
#define PS(k, v) do { json_builder_set_member_name (b, k); json_builder_add_string_value (b, (v) ? (v) : ""); } while (0)
#define PI(k, v) do { json_builder_set_member_name (b, k); json_builder_add_int_value (b, v); } while (0)
      PS ("id", r->id);
      PS ("title", r->title);
      PS ("driveName", r->drive_name);
      PS ("discName", r->disc_name);
      PS ("mode", r->mode);
      PS ("state", r->state);
      if (r->output_dir) PS ("outputDirectory", r->output_dir);
      if (r->error) PS ("errorMessage", r->error);
      PS ("logPath", r->log_path);
      PI ("startedAt", r->started_at);
      PI ("finishedAt", r->finished_at);
      PI ("warnings", r->warnings);
      PI ("errors", r->errors);
#undef PS
#undef PI
      json_builder_set_member_name (b, "files");
      json_builder_begin_array (b);
      for (guint k = 0; k < r->files->len; k++)
        json_builder_add_string_value (b, r->files->pdata[k]);
      json_builder_end_array (b);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  root = json_builder_get_root (b);
  text = bro_json_to_string (root, TRUE);
  bro_write_file_atomic (path, text, NULL);
}

/* ---- config ---- */

static gboolean
save_timeout (gpointer data)
{
  BroState *self = data;
  self->save_source = 0;
  bro_app_config_save (self->config, self->config_path, NULL);
  return G_SOURCE_REMOVE;
}

static void ensure_sessions (BroState *self);

void
bro_state_config_changed (BroState *self)
{
  g_clear_handle_id (&self->save_source, g_source_remove);
  self->save_source = g_timeout_add (400, save_timeout, self);
  ensure_sessions (self);
  emit (self, STATE_DRIVES_CHANGED);
}

void
bro_state_save_now (BroState *self)
{
  g_clear_handle_id (&self->save_source, g_source_remove);
  bro_app_config_save (self->config, self->config_path, NULL);
  save_history (self);
}

char *
bro_state_makemkvcon (BroState *self)
{
  return bro_find_tool (self->config->makemkvcon_path, "makemkvcon");
}

char *
bro_state_mkvmerge (BroState *self)
{
  return bro_find_tool (self->config->mkvmerge_path, "mkvmerge");
}

void
bro_state_set_error (BroState *self, const char *error)
{
  g_free (self->last_error);
  self->last_error = g_strdup (error);
  emit (self, STATE_STATUS_CHANGED);
}

void
bro_state_import_settings (BroState *self, GHashTable *settings)
{
  BroCatalog *cat = bro_catalog_get ();
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, settings);
  while (g_hash_table_iter_next (&it, &k, &v))
    if (bro_catalog_has_key (cat, k) && !g_str_equal (k, "app_DataDir"))
      g_hash_table_replace (self->config->global_settings, g_strdup (k), g_strdup (v));
  bro_state_config_changed (self);
}

BroState *
bro_state_new (GApplication *app)
{
  BroState *self = g_object_new (BRO_TYPE_STATE, NULL);
  g_autofree char *cfgdir = g_build_filename (g_get_user_config_dir (), "bromelia", NULL);
  self->app = app;
  self->config_path = g_build_filename (cfgdir, "config.json", NULL);
  if (g_file_test (self->config_path, G_FILE_TEST_EXISTS))
    self->config = bro_app_config_load (self->config_path);
  else
    {
      /* First run: import MakeMKV's own settings. */
      g_autoptr (GHashTable) installed = bro_makemkv_installed_settings ();
      const char *dest;
      self->config = bro_app_config_new ();
      bro_state_import_settings (self, installed);
      if ((dest = g_hash_table_lookup (installed, "app_DestinationDir")) && *dest)
        {
          g_free (self->config->output_root);
          self->config->output_root = g_strdup (dest);
        }
      bro_app_config_save (self->config, self->config_path, NULL);
    }
  load_history (self);
  return self;
}

/* ---- drives ---- */

GPtrArray *
bro_state_drive_items (BroState *self)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_item_free);
  g_autoptr (GHashTable) used = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < self->drives->len; i++)
    {
      BroDriveEntry *e = self->drives->pdata[i];
      BroDriveItem *it = g_new0 (BroDriveItem, 1);
      it->entry = bro_drive_entry_copy (e);
      it->config = bro_app_config_drive_for_entry (self->config, e);
      it->lane = bro_drive_entry_lane (e);
      it->id = g_strdup (it->config ? it->config->id : it->lane);
      if (it->config)
        g_hash_table_add (used, it->config->id);
      g_ptr_array_add (items, it);
    }
  for (guint i = 0; i < self->config->drives->len; i++)
    {
      BroDriveConfig *c = self->config->drives->pdata[i];
      BroDriveItem *it;
      if (g_hash_table_contains (used, c->id))
        continue;
      it = g_new0 (BroDriveItem, 1);
      it->config = c;
      it->id = g_strdup (c->id);
      it->lane = g_strdup_printf ("cfg:%s", c->id);
      g_ptr_array_add (items, it);
    }
  return items;
}

BroDriveItem *
bro_state_drive_item (BroState *self, const char *id)
{
  g_autoptr (GPtrArray) items = bro_state_drive_items (self);
  for (guint i = 0; i < items->len; i++)
    {
      BroDriveItem *it = items->pdata[i];
      if (g_str_equal (it->id, id))
        return g_ptr_array_steal_index (items, i);
    }
  return NULL;
}

BroDriveEntry *
bro_state_entry_for_lane (BroState *self, const char *lane)
{
  for (guint i = 0; i < self->drives->len; i++)
    {
      g_autofree char *l = bro_drive_entry_lane (self->drives->pdata[i]);
      if (g_str_equal (l, lane))
        return self->drives->pdata[i];
    }
  return NULL;
}

static void
ensure_sessions (BroState *self)
{
  for (guint i = 0; i < self->drives->len; i++)
    {
      BroDriveEntry *e = self->drives->pdata[i];
      g_autofree char *lane = bro_drive_entry_lane (e);
      BroDriveConfig *c = bro_app_config_drive_for_entry (self->config, e);
      const char *cfg_id = c ? c->id : self->config->default_drive->id;
      BroSession *s = g_hash_table_lookup (self->sessions, lane);
      if (s)
        {
          bro_source_free (s->source);
          s->source = bro_source_new_drive (e->index, e->device);
          if (g_strcmp0 (s->config_id, cfg_id) != 0)
            {
              g_free (s->config_id);
              s->config_id = g_strdup (cfg_id);
            }
        }
      else
        g_hash_table_insert (self->sessions, g_strdup (lane), session_new (lane, bro_source_new_drive (e->index, e->device), cfg_id));
    }
}

BroSession *
bro_state_session (BroState *self, const char *id)
{
  return g_hash_table_lookup (self->sessions, id);
}

BroDriveConfig *
bro_state_session_config (BroState *self, BroSession *s)
{
  BroDriveConfig *c = bro_app_config_drive_by_id (self->config, s->config_id);
  return c ? c : self->config->default_drive;
}

static BroJob *
make_drive_job (BroDriveEntry *e, const BroDriveConfig *cfg, BroRipMode mode)
{
  g_autofree char *lane = bro_drive_entry_lane (e);
  return job_new (bro_source_new_drive (e->index, e->device), cfg, lane, cfg->name, e->disc_name, mode);
}

static void enqueue (BroState *self, BroJob *job);

static void
disc_inserted (BroState *self, BroDriveEntry *e)
{
  g_autofree char *lane = bro_drive_entry_lane (e);
  BroSession *s = g_hash_table_lookup (self->sessions, lane);
  BroDriveConfig *cfg = bro_app_config_drive_for_entry (self->config, e);
  BroJob *job;
  if (s)
    bro_session_reset (s);
  if (!cfg || !cfg->enabled || !cfg->automation.auto_rip_on_insert)
    return;
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      if (g_str_equal (j->lane, lane) && !bro_job_state_finished (j->state))
        return;
    }
  job = make_drive_job (e, cfg, cfg->rip.mode);
  job->automatic = TRUE;
  job->start_at = g_get_real_time () / G_USEC_PER_SEC + MAX (0, cfg->automation.auto_rip_delay_seconds);
  job->state = BRO_JOB_WAITING;
  g_free (job->phase);
  job->phase = g_strdup ("Automatic rip");
  enqueue (self, job);
}

static gboolean
lane_running (BroState *self, const char *lane)
{
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      if (j->state == BRO_JOB_RUNNING && g_str_equal (j->lane, lane))
        return TRUE;
    }
  return FALSE;
}

void
bro_state_apply_scan (BroState *self, GPtrArray *entries)
{
  GPtrArray *merged = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_entry_free);
  for (guint i = 0; i < entries->len; i++)
    {
      BroDriveEntry *e = entries->pdata[i];
      g_autofree char *lane = NULL;
      BroDriveEntry *old;
      if (!bro_drive_entry_present (e))
        continue;
      lane = bro_drive_entry_lane (e);
      old = bro_state_entry_for_lane (self, lane);
      /* Keep the last known state of drives busy with a job. */
      g_ptr_array_add (merged, bro_drive_entry_copy (lane_running (self, lane) && old ? old : e));
    }
  g_ptr_array_unref (self->drives);
  self->drives = merged;
  ensure_sessions (self);

  for (guint i = 0; i < self->drives->len; i++)
    {
      BroDriveEntry *e = self->drives->pdata[i];
      char *lane = bro_drive_entry_lane (e);
      gpointer prev_p;
      gboolean had = g_hash_table_lookup_extended (self->known_states, lane, NULL, &prev_p);
      int prev = had ? GPOINTER_TO_INT (prev_p) : -1;
      g_hash_table_replace (self->known_states, lane, GINT_TO_POINTER (e->state));
      if (!self->first_scan_done)
        continue;
      if (e->state == BRO_DRIVE_INSERTED && prev != BRO_DRIVE_INSERTED)
        disc_inserted (self, e);
      else if (e->state != BRO_DRIVE_INSERTED && prev == BRO_DRIVE_INSERTED)
        {
          BroSession *s = g_hash_table_lookup (self->sessions, lane);
          if (s)
            bro_session_reset (s);
        }
    }
  self->first_scan_done = TRUE;
  emit (self, STATE_DRIVES_CHANGED);
}

typedef struct {
  char *exe;
  BroAppConfig *config;
  char *home;
  GPtrArray *entries;
  GPtrArray *messages;
  char *version;
  char *error;
} ScanData;

static void
scan_data_free (ScanData *d)
{
  g_free (d->exe);
  bro_app_config_free (d->config);
  g_free (d->home);
  if (d->entries) g_ptr_array_unref (d->entries);
  if (d->messages) g_ptr_array_unref (d->messages);
  g_free (d->version);
  g_free (d->error);
  g_free (d);
}

static void
scan_line (const char *line, gpointer data)
{
  ScanData *d = data;
  g_autoptr (BroEvent) ev = bro_event_parse (line);
  if (!ev)
    return;
  if (ev->type == BRO_EV_DRIVE)
    g_ptr_array_add (d->entries, bro_drive_entry_copy (&ev->drive));
  else if (ev->type == BRO_EV_MESSAGE)
    {
      if (ev->code == 1005 && ev->params->len)
        {
          g_free (d->version);
          d->version = g_strdup (ev->params->pdata[0]);
        }
      if (bro_event_severity (ev) != BRO_SEV_DEBUG && ev->code != 5010 && ev->code != 5042 && ev->code != 1005 && ev->code != 1004)
        g_ptr_array_add (d->messages, g_strdup (ev->text));
    }
}

static void
scan_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  ScanData *d = task_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (BroMakemkvEnv) env = bro_makemkv_env_prepare (d->exe, d->config, d->config->default_drive, d->home, NULL, &error);
  g_autoptr (GPtrArray) args = bro_makemkv_scan_args ();
  g_auto (GStrv) envp = NULL;
  if (!env)
    {
      d->error = g_strdup (error->message);
      g_task_return_boolean (task, FALSE);
      return;
    }
  envp = bro_makemkv_env_environ (env);
  g_ptr_array_insert (args, 0, g_strdup (d->exe));
  g_ptr_array_add (args, NULL);
  if (!bro_process_run ((const char *const *) args->pdata, (const char *const *) envp, d->home, 180, NULL, scan_line, d,
                        NULL, NULL, NULL, &error))
    d->error = g_strdup (error->message);
  g_task_return_boolean (task, TRUE);
}

static void
scan_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  ScanData *d = g_task_get_task_data (G_TASK (res));
  self->scanning = FALSE;
  self->last_scan = g_get_real_time () / G_USEC_PER_SEC;
  if (d->error)
    {
      g_autofree char *msg = g_strdup_printf ("Drive scan failed: %s", d->error);
      bro_state_set_error (self, msg);
    }
  else
    {
      if (d->version)
        {
          g_free (self->makemkv_version);
          self->makemkv_version = g_strdup (d->version);
        }
      g_ptr_array_unref (self->scan_messages);
      self->scan_messages = g_ptr_array_ref (d->messages);
      bro_state_apply_scan (self, d->entries);
    }
  emit (self, STATE_STATUS_CHANGED);
}

void
bro_state_refresh_drives (BroState *self, gboolean force)
{
  g_autofree char *exe = NULL;
  gboolean busy = FALSE;
  ScanData *d;
  GTask *task;
  if (self->scanning)
    return;
  exe = bro_state_makemkvcon (self);
  if (!exe)
    {
      bro_state_set_error (self, "makemkvcon was not found. Install MakeMKV or set its location in Preferences.");
      return;
    }
  for (guint i = 0; i < self->jobs->len; i++)
    busy |= ((BroJob *) self->jobs->pdata[i])->state == BRO_JOB_RUNNING;
  if (busy && !force && !self->config->poll_while_ripping)
    return;
  self->scanning = TRUE;
  emit (self, STATE_STATUS_CHANGED);
  d = g_new0 (ScanData, 1);
  d->exe = g_steal_pointer (&exe);
  d->config = bro_app_config_copy (self->config);
  {
    g_autofree char *data = bro_data_dir ();
    d->home = g_build_filename (data, "scanner", NULL);
  }
  d->entries = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_entry_free);
  d->messages = g_ptr_array_new_with_free_func (g_free);
  task = g_task_new (self, NULL, scan_done, NULL);
  g_task_set_task_data (task, d, (GDestroyNotify) scan_data_free);
  g_task_run_in_thread (task, scan_thread);
  g_object_unref (task);
}

static gboolean
rescan_timeout (gpointer data)
{
  BroState *self = data;
  self->rescan_source = 0;
  bro_state_refresh_drives (self, TRUE);
  return G_SOURCE_REMOVE;
}

void
bro_state_schedule_rescan (BroState *self, guint seconds)
{
  g_clear_handle_id (&self->rescan_source, g_source_remove);
  self->rescan_source = g_timeout_add_seconds (seconds, rescan_timeout, self);
}

BroDriveConfig *
bro_state_configure_drive (BroState *self, const BroDriveEntry *entry)
{
  BroDriveConfig *c = bro_app_config_drive_for_entry (self->config, entry);
  if (c)
    return c;
  c = bro_drive_config_copy (self->config->default_drive);
  g_free (c->id);
  c->id = g_uuid_string_random ();
  for (guint i = 0; i < c->post_process->len; i++)
    {
      BroPostStep *s = c->post_process->pdata[i];
      g_free (s->id);
      s->id = g_uuid_string_random ();
    }
  g_free (c->name);
  c->name = bro_drive_short_model (entry->drive_name);
  g_free (c->match_drive_name);
  c->match_drive_name = g_strdup (entry->drive_name);
  g_free (c->match_device);
  c->match_device = g_strdup (entry->device);
  g_ptr_array_add (self->config->drives, c);
  bro_state_config_changed (self);
  return c;
}

void
bro_state_remove_drive_config (BroState *self, const char *id)
{
  for (guint i = 0; i < self->config->drives->len; i++)
    if (g_str_equal (((BroDriveConfig *) self->config->drives->pdata[i])->id, id))
      {
        g_ptr_array_remove_index (self->config->drives, i);
        break;
      }
  bro_state_config_changed (self);
}

static void
eject_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  const char *device = task_data;
  g_autofree char *eject = g_find_program_in_path ("eject");
  int status = -1;
  if (eject)
    {
      const char *argv[] = { eject, device, NULL };
      bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, &status, NULL, NULL, NULL);
    }
  g_task_return_boolean (task, status == 0);
}

static void
eject_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  if (!g_task_propagate_boolean (G_TASK (res), NULL))
    {
      g_autofree char *msg = g_strdup_printf ("Could not eject %s", (char *) g_task_get_task_data (G_TASK (res)));
      bro_state_set_error (self, msg);
    }
  bro_state_schedule_rescan (self, 3);
}

void
bro_state_eject (BroState *self, const char *lane)
{
  BroDriveEntry *e = bro_state_entry_for_lane (self, lane);
  BroSession *s = g_hash_table_lookup (self->sessions, lane);
  GTask *task;
  if (!e || !e->device || !*e->device)
    return;
  if (s)
    bro_session_reset (s);
  task = g_task_new (self, NULL, eject_done, NULL);
  g_task_set_task_data (task, g_strdup (e->device), g_free);
  g_task_run_in_thread (task, eject_thread);
  g_object_unref (task);
}

/* ---- file sessions ---- */

BroSession *
bro_state_open_file (BroState *self, const char *path)
{
  gboolean dir = g_file_test (path, G_FILE_TEST_IS_DIR);
  g_autofree char *p = g_strdup (path);
  g_autofree char *base = NULL;
  BroSource *src;
  g_autofree char *key = NULL;
  BroSession *s;
  size_t n = strlen (p);
  while (n > 1 && p[n - 1] == '/')
    p[--n] = '\0';
  base = g_path_get_basename (p);
  if (dir && (g_ascii_strcasecmp (base, "BDMV") == 0 || g_ascii_strcasecmp (base, "VIDEO_TS") == 0))
    {
      char *parent = g_path_get_dirname (p);
      g_free (p);
      p = parent;
    }
  src = bro_source_new_path (dir ? BRO_SOURCE_FOLDER : BRO_SOURCE_ISO, p);
  key = bro_source_info_argument (src);
  s = g_hash_table_lookup (self->sessions, key);
  if (!s)
    {
      s = session_new (key, src, self->config->default_drive->id);
      g_hash_table_insert (self->sessions, g_strdup (key), s);
      g_ptr_array_add (self->file_sessions, g_object_ref (s));
    }
  else
    bro_source_free (src);
  emit (self, STATE_DRIVES_CHANGED);
  bro_state_load_disc (self, s);
  return s;
}

void
bro_state_close_file (BroState *self, BroSession *s)
{
  g_autofree char *id = g_strdup (s->id);
  bro_session_reset (s);
  g_ptr_array_remove (self->file_sessions, s);
  g_hash_table_remove (self->sessions, id);
  emit (self, STATE_DRIVES_CHANGED);
}

/* ---- loading a disc (info) ---- */

typedef struct {
  BroSession *session;
  BroEvent *event;
} SessionEvent;

typedef struct {
  BroSession *session;
  char *exe;
  BroAppConfig *config;
  BroDriveConfig *drive;
  char *home;
  gboolean show_debug;
  int exit_status;
  gboolean cancelled;
  char *error;
  BroDiscInfo *info;
  GPtrArray *errors;
  gint64 last_progress;
} LoadData;

static void
load_data_free (LoadData *d)
{
  g_object_unref (d->session);
  g_free (d->exe);
  bro_app_config_free (d->config);
  bro_drive_config_free (d->drive);
  g_free (d->home);
  g_free (d->error);
  if (d->info) bro_disc_info_unref (d->info);
  g_ptr_array_unref (d->errors);
  g_free (d);
}

static gboolean
apply_session_event (gpointer data)
{
  SessionEvent *se = data;
  BroSession *s = se->session;
  BroEvent *ev = se->event;
  switch (ev->type)
    {
    case BRO_EV_MESSAGE:
      session_log (s, bro_event_severity (ev), ev->text);
      break;
    case BRO_EV_PROGRESS_CURRENT:
      g_free (s->operation);
      s->operation = g_strdup (ev->text);
      session_changed (s);
      break;
    case BRO_EV_PROGRESS_VALUE:
      if (ev->max > 0)
        {
          s->progress = (double) ev->total / ev->max;
          session_changed (s);
        }
      break;
    case BRO_EV_RAW:
      session_log (s, BRO_SEV_INFO, ev->text);
      break;
    default:
      break;
    }
  return G_SOURCE_REMOVE;
}

static void
session_event_free (SessionEvent *se)
{
  g_object_unref (se->session);
  bro_event_free (se->event);
  g_free (se);
}

static void
load_line (const char *line, gpointer data)
{
  LoadData *d = data;
  BroEvent *ev = bro_event_parse (line);
  SessionEvent *se;
  if (!ev)
    return;
  bro_disc_info_consume (d->info, ev);
  if (ev->type == BRO_EV_MESSAGE && bro_event_severity (ev) == BRO_SEV_ERROR)
    g_ptr_array_add (d->errors, g_strdup (ev->text));
  if ((ev->type == BRO_EV_MESSAGE && bro_event_severity (ev) == BRO_SEV_DEBUG && !d->show_debug) ||
      ev->type == BRO_EV_CINFO || ev->type == BRO_EV_TINFO || ev->type == BRO_EV_SINFO || ev->type == BRO_EV_DRIVE)
    {
      bro_event_free (ev);
      return;
    }
  if (ev->type == BRO_EV_PROGRESS_VALUE)
    {
      gint64 now = g_get_monotonic_time ();
      if (now - d->last_progress < 100000)
        {
          bro_event_free (ev);
          return;
        }
      d->last_progress = now;
    }
  se = g_new0 (SessionEvent, 1);
  se->session = g_object_ref (d->session);
  se->event = ev;
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT_IDLE, apply_session_event, se, (GDestroyNotify) session_event_free);
}

static void
load_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  LoadData *d = task_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (BroMakemkvEnv) env = bro_makemkv_env_prepare (d->exe, d->config, d->drive, d->home, NULL, &error);
  g_autoptr (GPtrArray) args = NULL;
  g_auto (GStrv) envp = NULL;
  if (!env)
    {
      d->error = g_strdup (error->message);
      g_task_return_boolean (task, FALSE);
      return;
    }
  d->show_debug = g_strcmp0 (g_hash_table_lookup (env->settings, "app_ShowDebug"), "1") == 0;
  args = bro_makemkv_info_args (env, d->session->source, &d->drive->rip);
  g_ptr_array_add (args, NULL);
  envp = bro_makemkv_env_environ (env);
  {
    g_autofree char *cmd = bro_command_line ((const char *const *) args->pdata);
    g_autofree char *line = g_strdup_printf ("$ %s", cmd);
    load_line (line, d);
  }
  if (!bro_process_run ((const char *const *) args->pdata, (const char *const *) envp, d->home, 0, cancellable, load_line, d,
                        &d->exit_status, &d->cancelled, NULL, &error))
    d->error = g_strdup (error->message);
  g_task_return_boolean (task, TRUE);
}

static void
load_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  LoadData *d = g_task_get_task_data (G_TASK (res));
  BroSession *s = d->session;
  if (s->cancellable != g_task_get_cancellable (G_TASK (res)))
    return; /* superseded by a reset */
  g_clear_object (&s->cancellable);
  s->loading = FALSE;
  if (d->cancelled)
    s->error = g_strdup ("Cancelled");
  else if (d->error)
    s->error = g_strdup (d->error);
  else if (d->info->titles->len == 0)
    s->error = d->errors->len ? g_strdup (g_ptr_array_index (d->errors, d->errors->len - 1))
                              : g_strdup_printf ("No titles found (exit status %d).", d->exit_status);
  else
    {
      s->info = bro_disc_info_ref (d->info);
      bro_session_apply_rule (s, &bro_state_session_config (self, s)->rip.titles);
    }
  session_changed (s);
  emit (self, STATE_DRIVES_CHANGED);
}

void
bro_state_load_disc (BroState *self, BroSession *s)
{
  g_autofree char *exe = bro_state_makemkvcon (self);
  LoadData *d;
  GTask *task;
  if (s->loading)
    return;
  if (!exe)
    {
      g_free (s->error);
      s->error = g_strdup ("makemkvcon not found");
      session_changed (s);
      return;
    }
  if (s->source->kind == BRO_SOURCE_DRIVE && lane_running (self, s->id))
    {
      g_free (s->error);
      s->error = g_strdup ("The drive is busy with a job.");
      session_changed (s);
      return;
    }
  bro_session_reset (s);
  s->loading = TRUE;
  g_free (s->operation);
  s->operation = g_strdup ("Opening disc");
  s->cancellable = g_cancellable_new ();
  session_changed (s);

  d = g_new0 (LoadData, 1);
  d->session = g_object_ref (s);
  d->exe = g_steal_pointer (&exe);
  d->config = bro_app_config_copy (self->config);
  d->drive = bro_drive_config_copy (bro_state_session_config (self, s));
  {
    g_autofree char *data = bro_data_dir ();
    g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, s->id, -1);
    g_autofree char *short_hash = g_strndup (hash, 12);
    d->home = g_build_filename (data, "sessions", short_hash, NULL);
  }
  d->info = bro_disc_info_new ();
  d->errors = g_ptr_array_new_with_free_func (g_free);
  task = g_task_new (self, s->cancellable, load_done, NULL);
  g_task_set_priority (task, G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data (task, d, (GDestroyNotify) load_data_free);
  g_task_run_in_thread (task, load_thread);
  g_object_unref (task);
}

/* ---- jobs ---- */

BroJob *
bro_state_active_job (BroState *self, const char *lane)
{
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      if (g_str_equal (j->lane, lane) && !bro_job_state_finished (j->state))
        return j;
    }
  return NULL;
}

BroJob *
bro_state_recent_job (BroState *self, const char *lane)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  for (guint i = self->jobs->len; i > 0; i--)
    {
      BroJob *j = self->jobs->pdata[i - 1];
      if (g_str_equal (j->lane, lane) && (!bro_job_state_finished (j->state) || now - j->finished_at < 600))
        return j;
    }
  return NULL;
}

int
bro_state_active_count (BroState *self)
{
  int n = 0;
  for (guint i = 0; i < self->jobs->len; i++)
    n += ((BroJob *) self->jobs->pdata[i])->state == BRO_JOB_RUNNING;
  return n;
}

static void pump (BroState *self);

static void
enqueue (BroState *self, BroJob *job)
{
  g_ptr_array_add (self->jobs, job);
  emit (self, STATE_JOBS_CHANGED);
  emit (self, STATE_DRIVES_CHANGED);
  pump (self);
}

void
bro_state_quick_rip (BroState *self, BroDriveItem *item, int mode)
{
  const BroDriveConfig *cfg = item->config ? item->config : self->config->default_drive;
  BroSession *s;
  BroJob *job;
  if (!item->entry)
    return;
  job = make_drive_job (item->entry, cfg, mode >= 0 ? (BroRipMode) mode : cfg->rip.mode);
  s = g_hash_table_lookup (self->sessions, item->lane);
  if (s && s->info)
    {
      job->preloaded = bro_disc_info_ref (s->info);
      g_free (job->disc_label);
      job->disc_label = g_strdup (bro_disc_info_name (s->info));
    }
  enqueue (self, job);
}

void
bro_state_rip_session (BroState *self, BroSession *s, BroRipMode mode)
{
  BroDriveConfig *cfg = bro_state_session_config (self, s);
  g_autofree char *label = s->source->kind == BRO_SOURCE_DRIVE ? g_strdup (cfg->name) : bro_source_display_name (s->source);
  BroJob *job = job_new (bro_source_copy (s->source), cfg, s->id, label, s->info ? bro_disc_info_name (s->info) : "", mode);
  if (s->info)
    job->preloaded = bro_disc_info_ref (s->info);
  if (bro_rip_mode_makes_mkv (mode))
    {
      GHashTableIter it;
      gpointer k, v;
      job->manual_titles = g_array_new (FALSE, FALSE, sizeof (int));
      for (guint i = 0; s->info && i < s->info->titles->len; i++)
        {
          int t = ((BroTitle *) s->info->titles->pdata[i])->index;
          if (g_hash_table_contains (s->selected, GINT_TO_POINTER (t)))
            g_array_append_val (job->manual_titles, t);
        }
      g_hash_table_iter_init (&it, s->track_selections);
      while (g_hash_table_iter_next (&it, &k, &v))
        if (g_hash_table_contains (s->selected, k))
          {
            GHashTable *copy = bro_int_set_new ();
            GHashTableIter it2;
            gpointer tk;
            g_hash_table_iter_init (&it2, v);
            while (g_hash_table_iter_next (&it2, &tk, NULL))
              g_hash_table_add (copy, tk);
            g_hash_table_insert (job->track_selections, k, copy);
          }
      g_hash_table_iter_init (&it, s->name_overrides);
      while (g_hash_table_iter_next (&it, &k, &v))
        if (g_hash_table_contains (s->selected, k) && v && *(char *) v)
          g_hash_table_insert (job->name_overrides, k, g_strdup (v));
    }
  if (s->output_override && *s->output_override)
    {
      g_free (job->drive->output.root_override);
      job->drive->output.root_override = g_strdup (s->output_override);
      g_free (job->drive->output.folder_template);
      job->drive->output.folder_template = g_strdup ("");
      job->drive->output.conflict_policy = BRO_CONFLICT_OVERWRITE;
    }
  enqueue (self, job);
}

static void
delete_recursive (GFile *f)
{
  g_autoptr (GFileEnumerator) en = g_file_enumerate_children (f, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
  GFileInfo *info;
  while (en && (info = g_file_enumerator_next_file (en, NULL, NULL)))
    {
      g_autoptr (GFile) child = g_file_get_child (f, g_file_info_get_name (info));
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        delete_recursive (child);
      else
        g_file_delete (child, NULL, NULL);
      g_object_unref (info);
    }
  g_file_delete (f, NULL, NULL);
}

static void
record_history (BroState *self, BroJob *j)
{
  BroHistoryRecord *r = g_new0 (BroHistoryRecord, 1);
  r->id = g_strdup (j->id);
  r->title = bro_job_title (j);
  r->drive_name = g_strdup (j->drive->name);
  r->disc_name = g_strdup (j->disc_label);
  r->mode = g_strdup (bro_rip_mode_to_string (j->mode));
  r->state = g_strdup (bro_job_state_word (j->state));
  r->output_dir = g_strdup (j->output_dir);
  r->error = g_strdup (j->error);
  r->log_path = bro_job_log_path (j);
  r->started_at = j->started_at;
  r->finished_at = j->finished_at;
  r->warnings = j->warnings;
  r->errors = j->errors;
  r->files = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < j->files->len; i++)
    g_ptr_array_add (r->files, g_strdup (j->files->pdata[i]));
  for (guint i = 0; i < self->history->len; i++)
    if (g_str_equal (((BroHistoryRecord *) self->history->pdata[i])->id, r->id))
      {
        g_ptr_array_remove_index (self->history, i);
        break;
      }
  g_ptr_array_insert (self->history, 0, r);
  while ((int) self->history->len > MAX (10, self->config->history_limit))
    {
      BroHistoryRecord *old = self->history->pdata[self->history->len - 1];
      g_autofree char *dir = g_path_get_dirname (old->log_path);
      if (g_str_has_suffix (dir, old->id))
        {
          g_autoptr (GFile) f = g_file_new_for_path (dir);
          delete_recursive (f);
        }
      g_ptr_array_remove_index (self->history, self->history->len - 1);
    }
  save_history (self);
  emit (self, STATE_HISTORY_CHANGED);
}

void
bro_state_clear_history (BroState *self)
{
  for (guint i = 0; i < self->history->len; i++)
    {
      BroHistoryRecord *r = self->history->pdata[i];
      g_autofree char *dir = r->log_path ? g_path_get_dirname (r->log_path) : NULL;
      if (dir && g_str_has_suffix (dir, r->id))
        {
          g_autoptr (GFile) f = g_file_new_for_path (dir);
          delete_recursive (f);
        }
    }
  g_ptr_array_set_size (self->history, 0);
  save_history (self);
  emit (self, STATE_HISTORY_CHANGED);
}

typedef struct {
  BroJob *job;
  BroUpdate u;
} JobUpdate;

static void
job_update_free (JobUpdate *ju)
{
  g_object_unref (ju->job);
  g_free (ju->u.text);
  g_free (ju);
}

static gboolean
apply_job_update (gpointer data)
{
  JobUpdate *ju = data;
  BroJob *j = ju->job;
  BroUpdate *u = &ju->u;
  switch (u->kind)
    {
    case BRO_UPDATE_LOG:
      job_log (j, u->severity, u->text);
      return G_SOURCE_REMOVE;
    case BRO_UPDATE_PHASE:
      g_free (j->phase);
      j->phase = g_strdup (u->text);
      break;
    case BRO_UPDATE_OPERATION:
      g_free (j->operation);
      j->operation = g_strdup (u->text);
      break;
    case BRO_UPDATE_TOTAL_OPERATION:
      g_free (j->total_operation);
      j->total_operation = g_strdup (u->text);
      break;
    case BRO_UPDATE_PROGRESS:
      j->current = u->current;
      j->total = u->total;
      break;
    case BRO_UPDATE_STEPS:
      j->step_index = u->step_index;
      j->step_count = u->step_count;
      break;
    case BRO_UPDATE_DISC_LABEL:
      g_free (j->disc_label);
      j->disc_label = g_strdup (u->text);
      break;
    case BRO_UPDATE_OUTPUT_DIR:
      g_free (j->output_dir);
      j->output_dir = g_strdup (u->text);
      break;
    case BRO_UPDATE_FILE:
      g_ptr_array_add (j->files, g_strdup (u->text));
      break;
    case BRO_UPDATE_COMMAND:
      g_ptr_array_add (j->commands, g_strdup (u->text));
      break;
    }
  job_changed (j);
  return G_SOURCE_REMOVE;
}

static void
on_runner_update (const BroUpdate *u, gpointer data)
{
  BroJob *job = data;
  JobUpdate *ju;
  /* Updates are posted below redraw priority so a busy rip never starves painting; progress values
   * are throttled to ~10 per second. Everything else must stay ordered, so it is never dropped. */
  if (u->kind == BRO_UPDATE_PROGRESS)
    {
      gint64 now = g_get_monotonic_time ();
      if (now - job->worker_last_progress < 100000 && u->total < 1.0 && u->current < 1.0)
        return;
      job->worker_last_progress = now;
    }
  ju = g_new0 (JobUpdate, 1);
  ju->job = g_object_ref (job);
  ju->u = *u;
  ju->u.text = g_strdup (u->text);
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT_IDLE, apply_job_update, ju, (GDestroyNotify) job_update_free);
}

static void
job_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  BroRunRequest *req = task_data;
  g_task_return_pointer (task, bro_run_job (req), (GDestroyNotify) bro_run_result_free);
}

static void
job_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  BroJob *j = data;
  BroRunResult *r = g_task_propagate_pointer (G_TASK (res), NULL);
  if (r)
    {
      j->state = r->status;
      g_free (j->error);
      j->error = g_strdup (r->error);
      g_free (j->output_dir);
      j->output_dir = g_strdup (r->output_dir);
      g_ptr_array_set_size (j->files, 0);
      for (guint i = 0; i < r->files->len; i++)
        g_ptr_array_add (j->files, g_strdup (r->files->pdata[i]));
      if (r->disc_label && *r->disc_label)
        {
          g_free (j->disc_label);
          j->disc_label = g_strdup (r->disc_label);
        }
      bro_run_result_free (r);
    }
  else
    j->state = BRO_JOB_FAILED;
  j->finished_at = g_get_real_time () / G_USEC_PER_SEC;
  g_free (j->phase);
  j->phase = g_strdup (bro_job_state_label (j->state));
  if (j->state == BRO_JOB_SUCCEEDED)
    j->total = j->current = 1;
  g_clear_object (&j->cancellable);
  job_changed (j);
  record_history (self, j);

  if (j->drive->automation.notify && self->app)
    {
      g_autoptr (GNotification) n = NULL;
      g_autofree char *title = g_strdup_printf ("%s %s: %s", bro_rip_mode_short (j->mode),
                                                j->state == BRO_JOB_SUCCEEDED ? "finished" : bro_job_state_label (j->state),
                                                *j->disc_label ? j->disc_label : j->source_label);
      g_autofree char *body = j->state == BRO_JOB_SUCCEEDED
                                ? g_strdup_printf ("%u item(s) saved to %s", j->files->len, j->output_dir ? j->output_dir : "")
                                : g_strdup (j->error ? j->error : "Failed");
      n = g_notification_new (title);
      g_notification_set_body (n, body);
      g_application_send_notification (self->app, NULL, n);
    }
  emit (self, STATE_JOBS_CHANGED);
  emit (self, STATE_DRIVES_CHANGED);
  bro_state_schedule_rescan (self, 3);
  pump (self);
  g_object_unref (j);
}

static void
start_job (BroState *self, BroJob *j)
{
  g_autofree char *exe = bro_state_makemkvcon (self);
  BroRunRequest *req;
  GTask *task;
  if (!exe)
    {
      j->state = BRO_JOB_FAILED;
      j->error = g_strdup ("makemkvcon not found");
      j->finished_at = g_get_real_time () / G_USEC_PER_SEC;
      job_changed (j);
      record_history (self, j);
      return;
    }
  /* Drive numbering may have changed since the job was queued. */
  if (j->source->kind == BRO_SOURCE_DRIVE && *j->source->path)
    for (guint i = 0; i < self->drives->len; i++)
      {
        BroDriveEntry *e = self->drives->pdata[i];
        if (g_strcmp0 (e->device, j->source->path) == 0)
          j->source->index = e->index;
      }
  j->state = BRO_JOB_RUNNING;
  j->started_at = g_get_real_time () / G_USEC_PER_SEC;
  g_free (j->phase);
  j->phase = g_strdup ("Starting");
  j->cancellable = g_cancellable_new ();

  req = bro_run_request_new ();
  req->job_id = g_strdup (j->id);
  req->job_dir = bro_job_dir (j);
  req->config = bro_app_config_copy (self->config);
  req->drive = bro_drive_config_copy (j->drive);
  req->source = bro_source_copy (j->source);
  req->disc_label = g_strdup (j->disc_label);
  req->mode = j->mode;
  req->preloaded = j->preloaded ? bro_disc_info_ref (j->preloaded) : NULL;
  if (j->manual_titles)
    {
      req->manual_titles = g_array_new (FALSE, FALSE, sizeof (int));
      g_array_append_vals (req->manual_titles, j->manual_titles->data, j->manual_titles->len);
    }
  {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, j->track_selections);
    while (g_hash_table_iter_next (&it, &k, &v))
      {
        GHashTable *copy = bro_int_set_new ();
        GHashTableIter it2;
        gpointer tk;
        g_hash_table_iter_init (&it2, v);
        while (g_hash_table_iter_next (&it2, &tk, NULL))
          g_hash_table_add (copy, tk);
        g_hash_table_insert (req->track_selections, k, copy);
      }
    g_hash_table_iter_init (&it, j->name_overrides);
    while (g_hash_table_iter_next (&it, &k, &v))
      g_hash_table_insert (req->name_overrides, k, g_strdup (v));
  }
  req->makemkvcon = g_steal_pointer (&exe);
  req->mkvmerge = bro_state_mkvmerge (self);
  g_object_unref (req->cancellable);
  req->cancellable = g_object_ref (j->cancellable);
  req->on_update = on_runner_update;
  req->user_data = j;

  job_changed (j);
  task = g_task_new (self, NULL, job_done, g_object_ref (j));
  /* Same priority as the progress updates, so the result is applied after all of them. */
  g_task_set_priority (task, G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data (task, req, (GDestroyNotify) bro_run_request_free);
  g_task_run_in_thread (task, job_thread);
  g_object_unref (task);
  emit (self, STATE_DRIVES_CHANGED);
}

static void
pump (BroState *self)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      if (j->state == BRO_JOB_WAITING && j->start_at && j->start_at <= now)
        {
          j->state = BRO_JOB_QUEUED;
          job_changed (j);
        }
      else if (j->state == BRO_JOB_RUNNING || j->state == BRO_JOB_WAITING)
        job_changed (j); /* refresh elapsed / countdown */
    }
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      BroSession *s;
      if (j->state != BRO_JOB_QUEUED)
        continue;
      if (self->config->max_concurrent_jobs > 0 && bro_state_active_count (self) >= self->config->max_concurrent_jobs)
        break;
      if (lane_running (self, j->lane))
        continue;
      s = g_hash_table_lookup (self->sessions, j->lane);
      if (s && s->loading)
        continue;
      start_job (self, j);
    }
}

void
bro_state_cancel_job (BroState *self, BroJob *job)
{
  if (job->state == BRO_JOB_RUNNING && job->cancellable)
    {
      g_free (job->phase);
      job->phase = g_strdup ("Cancelling…");
      g_cancellable_cancel (job->cancellable);
      job_changed (job);
    }
  else if (!bro_job_state_finished (job->state))
    {
      job->state = BRO_JOB_CANCELLED;
      g_free (job->phase);
      job->phase = g_strdup ("Cancelled");
      job->finished_at = g_get_real_time () / G_USEC_PER_SEC;
      job_changed (job);
      record_history (self, job);
      emit (self, STATE_JOBS_CHANGED);
      emit (self, STATE_DRIVES_CHANGED);
    }
}

void
bro_state_start_now (BroState *self, BroJob *job)
{
  if (job->state != BRO_JOB_WAITING)
    return;
  job->start_at = 0;
  job->state = BRO_JOB_QUEUED;
  pump (self);
}

void
bro_state_retry (BroState *self, BroJob *job)
{
  BroJob *j = job_new (bro_source_copy (job->source), job->drive, job->lane, job->source_label, job->disc_label, job->mode);
  GHashTableIter it;
  gpointer k, v;
  if (job->preloaded)
    j->preloaded = bro_disc_info_ref (job->preloaded);
  if (job->manual_titles)
    j->manual_titles = g_array_ref (job->manual_titles);
  g_hash_table_iter_init (&it, job->track_selections);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_insert (j->track_selections, k, g_hash_table_ref (v));
  g_hash_table_iter_init (&it, job->name_overrides);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_insert (j->name_overrides, k, g_strdup (v));
  enqueue (self, j);
}

void
bro_state_move_job (BroState *self, BroJob *job, int offset)
{
  guint i;
  int k;
  if (!g_ptr_array_find (self->jobs, job, &i))
    return;
  k = (int) i + offset;
  if (k < 0 || k >= (int) self->jobs->len)
    return;
  gpointer tmp = self->jobs->pdata[i];
  self->jobs->pdata[i] = self->jobs->pdata[k];
  self->jobs->pdata[k] = tmp;
  emit (self, STATE_JOBS_CHANGED);
}

void
bro_state_clear_finished (BroState *self)
{
  for (guint i = self->jobs->len; i > 0; i--)
    if (bro_job_state_finished (((BroJob *) self->jobs->pdata[i - 1])->state))
      g_ptr_array_remove_index (self->jobs, i - 1);
  emit (self, STATE_JOBS_CHANGED);
  emit (self, STATE_DRIVES_CHANGED);
}

void
bro_state_save_preset (BroState *self, const char *name, const BroDriveConfig *from)
{
  g_ptr_array_add (self->config->presets, bro_preset_new (name, from));
  bro_state_config_changed (self);
}

/* ---- registration ---- */

static void
reg_line (const char *line, gpointer data)
{
  g_autoptr (BroEvent) ev = bro_event_parse (line);
  g_string_append_printf (data, "%s\n", ev && ev->type == BRO_EV_MESSAGE ? ev->text : line);
}

static void
reg_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  char **argv = task_data;
  GString *out = g_string_new (NULL);
  int status = -1;
  g_autoptr (GError) error = NULL;
  if (!bro_process_run ((const char *const *) argv, NULL, NULL, 60, NULL, reg_line, out, &status, NULL, NULL, &error))
    g_string_append (out, error->message);
  else if (status != 0)
    g_string_prepend (out, "Registration failed: ");
  else if (out->len == 0)
    g_string_append (out, "Key registered.");
  g_task_return_pointer (task, g_string_free (out, FALSE), g_free);
}

void
bro_state_register_key (BroState *self, const char *key, GAsyncReadyCallback cb, gpointer data)
{
  g_autofree char *exe = bro_state_makemkvcon (self);
  GTask *task = g_task_new (self, NULL, cb, data);
  if (!exe)
    {
      g_task_return_pointer (task, g_strdup ("makemkvcon not found"), g_free);
      g_object_unref (task);
      return;
    }
  char **argv = g_new0 (char *, 4);
  argv[0] = g_steal_pointer (&exe);
  argv[1] = g_strdup ("reg");
  argv[2] = g_strdup (key);
  g_task_set_task_data (task, argv, (GDestroyNotify) g_strfreev);
  g_task_run_in_thread (task, reg_thread);
  g_object_unref (task);
}

char *
bro_state_register_key_finish (BroState *self, GAsyncResult *res)
{
  return g_task_propagate_pointer (G_TASK (res), NULL);
}

/* ---- start ---- */

static gboolean
tick (gpointer data)
{
  BroState *self = data;
  gboolean busy = FALSE;
  pump (self);
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJobState s = ((BroJob *) self->jobs->pdata[i])->state;
      busy |= s == BRO_JOB_RUNNING || s == BRO_JOB_WAITING;
    }
  if (busy)
    emit (self, STATE_TICK);
  return G_SOURCE_CONTINUE;
}

static gboolean
poll_timeout (gpointer data)
{
  BroState *self = data;
  if (self->config->poll_interval_seconds > 0)
    bro_state_refresh_drives (self, FALSE);
  self->poll_source = g_timeout_add_seconds (MAX (3, self->config->poll_interval_seconds), poll_timeout, self);
  return G_SOURCE_REMOVE;
}

static void
on_media_changed (BroState *self)
{
  bro_state_schedule_rescan (self, 2);
}

void
bro_state_start (BroState *self)
{
  if (self->tick_source)
    return;
  self->tick_source = g_timeout_add_seconds (1, tick, self);
  self->poll_source = g_timeout_add_seconds (MAX (3, self->config->poll_interval_seconds), poll_timeout, self);
  self->monitor = g_volume_monitor_get ();
  g_signal_connect_swapped (self->monitor, "drive-changed", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "drive-connected", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "drive-disconnected", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "volume-added", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "volume-removed", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "drive-eject-button", G_CALLBACK (on_media_changed), self);
  bro_state_refresh_drives (self, TRUE);
}
