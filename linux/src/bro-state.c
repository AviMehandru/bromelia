/* bro-state.c — application state, drive scanning, disc sessions, job queue and history. */
#include "bro-state.h"
#include "bro-integrations.h"
#include "bro-logic.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

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
  g_free (j->media_name);
  g_free (j->phase);
  g_free (j->operation);
  g_free (j->total_operation);
  g_free (j->output_dir);
  g_free (j->fingerprint);
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
  j->media_name = g_strdup ("");
  j->media_kind = -1;
  j->first_episode = -1;
  j->disc_flags = -1;
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
  g_free (s->media_name);
  g_free (s->libre_drive);
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
  s->media_name = g_strdup ("");
  s->media_kind = -1;
  s->first_episode = -1;
  s->disc_flags = -1;
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
  g_free (s->media_name);
  s->media_name = g_strdup ("");
  s->media_kind = -1;
  s->first_episode = -1;
  g_clear_pointer (&s->libre_drive, g_free);
  s->libre_drive_required = FALSE;
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

enum { STATE_DRIVES_CHANGED, STATE_JOBS_CHANGED, STATE_HISTORY_CHANGED, STATE_STATUS_CHANGED, STATE_TICK, STATE_VERIFY_CHANGED,
       STATE_N_SIGNALS };
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
  g_free (r->fingerprint);
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
background_item_free (BroBackgroundItem *b)
{
  g_free (b->id);
  bro_background_work_free (b->work);
  g_free (b->state);
  g_free (b->message);
  g_free (b);
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
  g_ptr_array_unref (self->background);
  bro_web_server_free (self->web);
  bro_state_set_sleep_inhibitor (self, NULL);
  g_free (self->unfinished_saved);
  bro_state_verify_cancel (self);
  g_free (self->verify.path);
  g_free (self->verify.folder);
  g_free (self->verify.file);
  if (self->verify.results) g_ptr_array_unref (self->verify.results);
  if (self->check_records) g_hash_table_unref (self->check_records);
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
  /* Progress and results of verifying archives (BroState.verify). */
  state_signals[STATE_VERIFY_CHANGED] = g_signal_new ("verify-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                                      G_TYPE_NONE, 0);
}

static void update_keep_awake (BroState *self);
static void save_unfinished (BroState *self);
static char *check_records_path (void);
static void delete_recursive (GFile *f);
static void add_history (BroState *self, BroHistoryRecord *r);

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
  self->background = g_ptr_array_new_with_free_func ((GDestroyNotify) background_item_free);
  g_signal_connect (self, "jobs-changed", G_CALLBACK (update_keep_awake), NULL);
  g_signal_connect (self, "jobs-changed", G_CALLBACK (save_unfinished), NULL);
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
      r->fingerprint = GS ("fingerprint");
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
      if (r->fingerprint) PS ("fingerprint", r->fingerprint);
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

/* ---- unfinished jobs ---- */

/* Identifies this process in unfinished-<pid>.json, so a file left by an earlier process with the same pid (pid 1 in
 * a container) is still recognised as stale. */
static const char *
instance_token (void)
{
  static char *token;
  if (!token)
    token = g_uuid_string_random ();
  return token;
}

static char *
unfinished_path (int pid)
{
  g_autofree char *dir = bro_data_dir ();
  g_autofree char *name = g_strdup_printf ("unfinished-%d.json", pid);
  return g_build_filename (dir, name, NULL);
}

static void
save_unfinished (BroState *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL, *path = unfinished_path (getpid ());
  int n = 0;
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "pid");
  json_builder_add_int_value (b, getpid ());
  json_builder_set_member_name (b, "instance");
  json_builder_add_string_value (b, instance_token ());
  json_builder_set_member_name (b, "jobs");
  json_builder_begin_array (b);
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      g_autofree char *title = NULL, *log = NULL;
      if (bro_job_state_finished (j->state))
        continue;
      n++;
      title = bro_job_title (j);
      log = bro_job_log_path (j);
      json_builder_begin_object (b);
#define PS(k, v) do { json_builder_set_member_name (b, k); json_builder_add_string_value (b, (v) ? (v) : ""); } while (0)
      PS ("id", j->id);
      PS ("title", title);
      PS ("driveName", j->drive ? j->drive->name : NULL);
      PS ("discName", j->disc_label);
      PS ("mode", bro_rip_mode_to_string (j->mode));
      PS ("state", j->state == BRO_JOB_RUNNING ? "running" : j->state == BRO_JOB_WAITING ? "waiting" : "queued");
      PS ("outputDirectory", j->output_dir);
      PS ("logPath", log);
#undef PS
      json_builder_set_member_name (b, "startedAt");
      json_builder_add_int_value (b, j->started_at);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  text = n ? bro_json_to_string (root, TRUE) : NULL;
  if (g_strcmp0 (text, self->unfinished_saved) == 0)
    return;
  if (text)
    {
      g_autofree char *dir = g_path_get_dirname (path);
      g_mkdir_with_parents (dir, 0700);
      bro_write_file_atomic (path, text, NULL);
    }
  else
    g_unlink (path);
  g_free (self->unfinished_saved);
  self->unfinished_saved = g_steal_pointer (&text);
}

static gboolean
process_alive (int pid)
{
  return pid > 0 && (kill (pid, 0) == 0 || errno == EPERM);
}

static gboolean
has_visible_items (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *n;
  while (d && (n = g_dir_read_name (d)))
    if (n[0] != '.')
      return TRUE;
  return FALSE;
}

/* The staging folder of a job that was running: made visible with a note, or removed when nothing was saved. Returns
 * where its files are now, or NULL. */
static char *
recover_staging (const char *output_dir, const char *id, const char *log_path)
{
  g_autofree char *short_id = g_ascii_strdown (id, MIN (strlen (id), 8));
  g_autofree char *stage_name = g_strconcat (BRO_STAGING_PREFIX, short_id, NULL);
  g_autofree char *stage = g_build_filename (output_dir, stage_name, NULL);
  g_autofree char *want = NULL, *note = NULL, *text = NULL;
  char *dest;
  if (!g_file_test (stage, G_FILE_TEST_IS_DIR))
    return NULL;
  if (!has_visible_items (stage))
    {
      g_autoptr (GFile) f = g_file_new_for_path (stage);
      delete_recursive (f); /* only temporary (hidden) files */
      g_rmdir (output_dir); /* the folder made for the job, if nothing else is in it */
      return NULL;
    }
  want = g_strdup_printf ("%s/INCOMPLETE - %s", output_dir, short_id);
  dest = bro_unique_path (want);
  if (g_rename (stage, dest) != 0)
    {
      g_free (dest);
      return g_steal_pointer (&stage);
    }
  note = g_build_filename (dest, "INCOMPLETE.txt", NULL);
  text = g_strdup_printf ("Bromelia job %s: interrupted.\n"
                          "Bromelia stopped (it quit, crashed or lost power) while this job was running.\n"
                          "These files are NOT a finished archive. Rip the disc again.\n\n"
                          "Log up to the interruption: %s\n", id, log_path ? log_path : "");
  g_file_set_contents (note, text, -1, NULL);
  return dest;
}

int
bro_state_recover_unfinished (BroState *self)
{
  g_autofree char *dir = bro_data_dir ();
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  const char *name;
  int recovered = 0;
  while (d && (name = g_dir_read_name (d)))
    if (g_str_has_prefix (name, "unfinished-") && g_str_has_suffix (name, ".json"))
      g_ptr_array_add (files, g_build_filename (dir, name, NULL));
  for (guint f = 0; f < files->len; f++)
    {
      const char *path = files->pdata[f];
      g_autoptr (JsonParser) p = json_parser_new ();
      JsonObject *o;
      JsonArray *jobs;
      if (!json_parser_load_from_file (p, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
        {
          g_unlink (path);
          continue;
        }
      o = json_node_get_object (json_parser_get_root (p));
      /* Still in use by another Bromelia (the app and the daemon can run side by side). */
      if (g_strcmp0 (json_object_get_string_member_with_default (o, "instance", ""), instance_token ()) == 0)
        continue;
      if ((int) json_object_get_int_member_with_default (o, "pid", 0) != getpid ()
          && process_alive ((int) json_object_get_int_member_with_default (o, "pid", 0)))
        continue;
      jobs = json_object_has_member (o, "jobs") ? json_object_get_array_member (o, "jobs") : NULL;
      for (guint i = 0; jobs && i < json_array_get_length (jobs); i++)
        {
          JsonObject *jo = json_array_get_object_element (jobs, i);
          BroHistoryRecord *r = g_new0 (BroHistoryRecord, 1);
          gboolean running = g_str_equal (json_object_get_string_member_with_default (jo, "state", ""), "running");
          const char *out = json_object_get_string_member_with_default (jo, "outputDirectory", "");
          g_autofree char *kept = NULL;
#define GS(k) g_strdup (json_object_get_string_member_with_default (jo, k, ""))
          r->id = GS ("id");
          r->title = GS ("title");
          r->drive_name = GS ("driveName");
          r->disc_name = GS ("discName");
          r->mode = GS ("mode");
          r->log_path = GS ("logPath");
#undef GS
          r->started_at = json_object_get_int_member_with_default (jo, "startedAt", 0);
          r->finished_at = g_get_real_time () / G_USEC_PER_SEC;
          r->files = g_ptr_array_new_with_free_func (g_free);
          r->errors = running;
          if (running)
            {
              kept = *out ? recover_staging (out, r->id, r->log_path) : NULL;
              r->state = g_strdup ("failed");
              r->output_dir = g_strdup (kept);
              r->error = kept ? g_strdup_printf ("Interrupted: Bromelia stopped while this job was running; its files were kept in %s", kept)
                              : g_strdup ("Interrupted: Bromelia stopped while this job was running; nothing was saved");
            }
          else
            {
              r->state = g_strdup ("cancelled");
              r->error = g_strdup ("Not started: Bromelia stopped while this job was waiting");
            }
          add_history (self, r);
          recovered++;
        }
      g_unlink (path);
    }
  if (recovered)
    {
      g_autofree char *msg = g_strdup_printf ("Bromelia stopped last time with %d unfinished job(s); they are in the history, "
                                              "marked as interrupted or not started", recovered);
      bro_state_set_error (self, msg);
    }
  return recovered;
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
static void pump_background (BroState *self);

void
bro_state_config_changed (BroState *self)
{
  g_clear_handle_id (&self->save_source, g_source_remove);
  self->save_source = g_timeout_add (400, save_timeout, self);
  ensure_sessions (self);
  if (self->web)
    bro_web_server_apply (self->web, &self->config->web_ui);
  pump_background (self);
  update_keep_awake (self);
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

static JsonNode *
web_status (gpointer data)
{
  return bro_state_web_status (data);
}

static char *
web_action (const char *kind, const char *id, const char *action, gpointer data)
{
  return bro_state_web_action (data, kind, id, action);
}

static BroState *
state_new_at (GApplication *app, const char *config_path)
{
  BroState *self = g_object_new (BRO_TYPE_STATE, NULL);
  self->app = app;
  self->config_path = config_path ? g_strdup (config_path)
                                  : g_build_filename (g_get_user_config_dir (), "bromelia", "config.json", NULL);
  self->web = bro_web_server_new (web_status, web_action, self);
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
  bro_state_recover_unfinished (self);
  {
    g_autofree char *path = check_records_path ();
    self->check_records = bro_check_records_load (path);
  }
  return self;
}

BroState *
bro_state_new (GApplication *app)
{
  return state_new_at (app, NULL);
}

static void
inhibitor_problem (const char *message, gpointer data)
{
  bro_state_set_error (data, message);
}

BroState *
bro_state_new_headless (const char *config_path)
{
  BroState *self = state_new_at (NULL, config_path);
  BroSleepInhibitor *logind = bro_sleep_inhibitor_logind_new ();
  logind->problem = inhibitor_problem;
  logind->problem_data = self;
  bro_state_set_sleep_inhibitor (self, logind);
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
          s->disc_flags = e->flags;
          if (g_strcmp0 (s->config_id, cfg_id) != 0)
            {
              g_free (s->config_id);
              s->config_id = g_strdup (cfg_id);
            }
        }
      else
        {
          s = session_new (lane, bro_source_new_drive (e->index, e->device), cfg_id);
          s->disc_flags = e->flags;
          g_hash_table_insert (self->sessions, g_strdup (lane), s);
        }
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
  BroJob *j = job_new (bro_source_new_drive (e->index, e->device), cfg, lane, cfg->name, e->disc_name, mode);
  j->disc_flags = e->flags;
  return j;
}

/* Name, movie / TV and first episode chosen on the disc page. */
static void
apply_identity_choices (BroSession *s, BroJob *j)
{
  g_free (j->media_name);
  j->media_name = g_strstrip (g_strdup (s->media_name ? s->media_name : ""));
  j->media_kind = s->media_kind;
  j->first_episode = s->first_episode;
  if (j->disc_flags < 0)
    j->disc_flags = s->disc_flags;
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
  {
    BroRipMode mode;
    if (!bro_disc_mode_for (e->flags, bro_disc_content_probe, e->device, cfg, &mode))
      return;
    job = make_drive_job (e, cfg, mode);
    job->uses_configured_mode = mode == cfg->rip.mode;
  }
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
  int problem;
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
      if (bro_notice_is_license_problem (bro_notice_from_event (ev, NULL)))
        d->problem = bro_notice_from_event (ev, NULL);
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
      if (d->problem != BRO_NOTICE_NONE)
        self->makemkv_problem = d->problem;
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
  /* A disc image, a disc folder, or the disc folder a file (.IFO, .mpls, .m2ts, …) belongs to. */
  BroSource *src = bro_source_resolve (path, g_file_test (path, G_FILE_TEST_IS_DIR));
  g_autofree char *key = NULL;
  BroSession *s;
  key = bro_source_info_argument (src);
  s = g_hash_table_lookup (self->sessions, key);
  if (!s)
    {
      s = session_new (key, src, self->config->default_drive->id);
      if (src->kind == BRO_SOURCE_FOLDER && !g_str_equal (src->path, path))
        {
          g_autofree char *name = g_path_get_basename (path);
          g_autofree char *msg = g_strdup_printf ("Opening the disc that %s belongs to: %s", name, src->path);
          session_log (s, BRO_SEV_INFO, msg);
        }
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
  char *libre_drive;
  gboolean libre_required;
  int problem;
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
  g_free (d->libre_drive);
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
  {
    g_autofree char *detail = NULL;
    BroNotice n = bro_notice_from_event (ev, &detail);
    if (n == BRO_NOTICE_LIBREDRIVE && !d->libre_drive)
      d->libre_drive = g_steal_pointer (&detail);
    else if (n == BRO_NOTICE_LIBREDRIVE_REQUIRED)
      d->libre_required = TRUE;
    else if (bro_notice_is_license_problem (n))
      d->problem = n;
  }
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
  if (d->problem != BRO_NOTICE_NONE)
    {
      self->makemkv_problem = d->problem;
      emit (self, STATE_STATUS_CHANGED);
    }
  if (s->source->kind == BRO_SOURCE_DRIVE)
    {
      g_free (s->libre_drive);
      s->libre_drive_required = d->libre_required;
      if (d->libre_required)
        s->libre_drive = g_strdup ("LibreDrive required: this drive can't decrypt this disc");
      else if (d->libre_drive)
        s->libre_drive = *d->libre_drive ? g_strdup_printf ("LibreDrive: enabled (%s)", d->libre_drive) : g_strdup ("LibreDrive: enabled");
      else
        s->libre_drive = g_strdup ("LibreDrive: not in use for this disc");
    }
  if (d->cancelled)
    s->error = g_strdup ("Cancelled");
  else if (d->error)
    s->error = g_strdup (d->error);
  else if (d->info->titles->len == 0)
    {
      if (bro_notice_is_license_problem (d->problem))
        s->error = g_strdup (bro_notice_explanation (d->problem));
      else if (s->source->kind == BRO_SOURCE_FOLDER && d->errors->len == 0)
        s->error = g_strdup ("MakeMKV found no titles here. It opens disc images and disc folders (with BDMV, VIDEO_TS or HVDVD_TS), "
                             "not single video files.");
      else
        s->error = d->errors->len ? g_strdup (g_ptr_array_index (d->errors, d->errors->len - 1))
                                  : g_strdup_printf ("No titles found (exit status %d).", d->exit_status);
    }
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
  /* The output folder is only known once the job has started. */
  g_signal_connect_object (job, "changed", G_CALLBACK (save_unfinished), self, G_CONNECT_SWAPPED);
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
  BroRipMode chosen = mode >= 0 ? (BroRipMode) mode : cfg->rip.mode;
  if (!item->entry)
    return;
  if (mode < 0 && !bro_disc_mode_for (item->entry->flags, bro_disc_content_probe, item->entry->device, cfg, &chosen))
    {
      g_autofree char *name = bro_drive_item_name (item);
      g_autofree char *msg = g_strdup_printf ("%s: this isn't a DVD or Blu-ray, and the drive is set to leave other discs alone", name);
      bro_state_set_error (self, msg);
      return;
    }
  job = make_drive_job (item->entry, cfg, chosen);
  job->uses_configured_mode = mode < 0 && chosen == cfg->rip.mode;
  s = g_hash_table_lookup (self->sessions, item->lane);
  if (s && s->info)
    {
      job->preloaded = bro_disc_info_ref (s->info);
      g_free (job->disc_label);
      job->disc_label = g_strdup (bro_disc_info_name (s->info));
      apply_identity_choices (s, job);
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
  apply_identity_choices (s, job);
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
  r->fingerprint = g_strdup (j->fingerprint);
  r->files = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < j->files->len; i++)
    g_ptr_array_add (r->files, g_strdup (j->files->pdata[i]));
  add_history (self, r);
}

/* Takes r. */
static void
add_history (BroState *self, BroHistoryRecord *r)
{
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
      j->mode = r->mode;
      if (bro_notice_is_license_problem (r->problem))
        {
          self->makemkv_problem = r->problem;
          emit (self, STATE_STATUS_CHANGED);
          if (r->problem == BRO_NOTICE_KEY_EXPIRED && self->config->auto_update_beta_key && !self->beta_key_tried)
            {
              self->beta_key_tried = TRUE;
              bro_state_update_beta_key_if_needed (self, "after it expired");
            }
        }
      if (r->background)
        bro_state_enqueue_background (self, g_steal_pointer (&r->background));
      g_free (j->error);
      j->error = g_strdup (r->error);
      g_free (j->output_dir);
      j->output_dir = g_strdup (r->output_dir);
      g_free (j->fingerprint);
      j->fingerprint = g_strdup (r->fingerprint);
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
      g_autofree char *title = NULL, *body = NULL;
      bro_notification_text (j->state, j->mode, *j->disc_label ? j->disc_label : j->source_label, j->files->len, j->output_dir,
                             j->error, &title, &body);
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
  req->automatic = j->automatic;
  req->uses_configured_mode = j->uses_configured_mode;
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
  g_free (req->media_name);
  req->media_name = g_strdup (j->media_name);
  req->media_kind = j->media_kind;
  req->first_episode = j->first_episode;
  req->disc_flags = j->disc_flags;
  req->makemkvcon = g_steal_pointer (&exe);
  req->mkvmerge = bro_state_mkvmerge (self);
  g_object_unref (req->cancellable);
  req->cancellable = g_object_ref (j->cancellable);
  req->on_update = on_runner_update;
  req->user_data = j;
  req->archived = bro_state_archived_candidates (self);

  job_changed (j);
  task = g_task_new (self, NULL, job_done, g_object_ref (j));
  /* Same priority as the progress updates, so the result is applied after all of them. */
  g_task_set_priority (task, G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data (task, req, (GDestroyNotify) bro_run_request_free);
  g_task_run_in_thread (task, job_thread);
  g_object_unref (task);
  update_keep_awake (self);
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
  g_free (j->media_name);
  j->media_name = g_strdup (job->media_name);
  j->media_kind = job->media_kind;
  j->first_episode = job->first_episode;
  j->disc_flags = job->disc_flags;
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

/* ---- beta key ---- */

static void
beta_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  const char *exe = task_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *key = bro_beta_key_fetch (&error);
  if (!key)
    {
      g_task_return_pointer (task, g_strdup_printf ("Could not get the beta key: %s", error->message), g_free);
      return;
    }
  {
    const char *argv[] = { exe, "reg", key, NULL };
    GString *out = g_string_new (NULL);
    int status = -1;
    g_autoptr (GError) err = NULL;
    if (!bro_process_run (argv, NULL, NULL, 60, NULL, reg_line, out, &status, NULL, NULL, &err) || status != 0)
      {
        g_autofree char *text = g_string_free (out, FALSE);
        g_task_return_pointer (task, g_strdup_printf ("Registration failed: %s", err ? err->message : text), g_free);
        return;
      }
    g_string_free (out, TRUE);
  }
  g_object_set_data_full (G_OBJECT (task), "key", g_strdup (key), g_free);
  {
    g_autofree char *head = g_strndup (key, 8);
    g_task_return_pointer (task, g_strdup_printf ("Registered the current beta key (%s…).", head), g_free);
  }
}

void
bro_state_install_beta_key (BroState *self, GAsyncReadyCallback cb, gpointer data)
{
  g_autofree char *exe = bro_state_makemkvcon (self);
  GTask *task = g_task_new (self, NULL, cb, data);
  if (!exe)
    {
      g_task_return_pointer (task, g_strdup ("makemkvcon not found"), g_free);
      g_object_unref (task);
      return;
    }
  g_task_set_task_data (task, g_steal_pointer (&exe), g_free);
  g_task_run_in_thread (task, beta_thread);
  g_object_unref (task);
}

char *
bro_state_install_beta_key_finish (BroState *self, GAsyncResult *res)
{
  const char *key = g_object_get_data (G_OBJECT (res), "key");
  if (key)
    {
      /* A key typed into Bromelia would override the new one for every drive. */
      if (*self->config->registration_key)
        {
          g_free (self->config->registration_key);
          self->config->registration_key = g_strdup (key);
          bro_state_config_changed (self);
        }
      bro_state_clear_problem (self);
    }
  return g_task_propagate_pointer (G_TASK (res), NULL);
}

void
bro_state_clear_problem (BroState *self)
{
  self->makemkv_problem = BRO_NOTICE_NONE;
  emit (self, STATE_STATUS_CHANGED);
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
  {
    gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    if (now >= self->next_schedule_check)
      {
        self->next_schedule_check = now + 3600;
        if (bro_state_verify_due (self, now) && bro_state_active_count (self) == 0)
          {
            g_autofree char *root = bro_state_output_root (self);
            bro_state_verify_start (self, root, TRUE);
          }
      }
  }
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
  /* The first scheduled archive check (archiveCheck.intervalDays) a minute after starting, then hourly. */
  self->next_schedule_check = g_get_real_time () / G_USEC_PER_SEC + 60;
  self->poll_source = g_timeout_add_seconds (MAX (3, self->config->poll_interval_seconds), poll_timeout, self);
  self->monitor = g_volume_monitor_get ();
  g_signal_connect_swapped (self->monitor, "drive-changed", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "drive-connected", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "drive-disconnected", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "volume-added", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "volume-removed", G_CALLBACK (on_media_changed), self);
  g_signal_connect_swapped (self->monitor, "drive-eject-button", G_CALLBACK (on_media_changed), self);
  bro_web_server_apply (self->web, &self->config->web_ui);
  if (self->config->auto_update_beta_key)
    bro_state_update_beta_key_if_needed (self, "at startup");
  bro_state_refresh_drives (self, TRUE);
}

/* ---- sleep ---- */

gboolean
bro_state_wants_awake (BroState *self)
{
  return self->config && self->config->prevent_sleep
         && (bro_state_active_count (self) > 0 || bro_state_background_busy (self) || self->verify.running);
}

static void
update_keep_awake (BroState *self)
{
  gboolean want = self->inhibitor && bro_state_wants_awake (self);
  if (want == self->keeping_awake)
    return;
  self->keeping_awake = want;
  self->inhibitor->set (self->inhibitor, want);
}

void
bro_state_set_sleep_inhibitor (BroState *self, BroSleepInhibitor *inhibitor)
{
  if (self->inhibitor && self->keeping_awake)
    self->inhibitor->set (self->inhibitor, FALSE);
  self->keeping_awake = FALSE;
  bro_sleep_inhibitor_free (self->inhibitor);
  self->inhibitor = inhibitor;
  if (inhibitor)
    update_keep_awake (self);
}

/* ---- archives: already archived, verifying ---- */

char *
bro_state_output_root (BroState *self)
{
  const char *root = self->config->output_root && *self->config->output_root ? self->config->output_root : "~/Videos/Bromelia";
  return g_str_has_prefix (root, "~") ? g_build_filename (g_get_home_dir (), root + 1, NULL) : g_strdup (root);
}

GPtrArray *
bro_state_archived_candidates (BroState *self)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_archived_candidate_free);
  for (guint i = 0; i < self->history->len; i++)
    {
      BroHistoryRecord *r = self->history->pdata[i];
      BroArchivedCandidate *c;
      if (!r->fingerprint || !*r->fingerprint || !r->output_dir)
        continue;
      c = g_new0 (BroArchivedCandidate, 1);
      c->fingerprint = g_strdup (r->fingerprint);
      c->folder = g_strdup (r->output_dir);
      c->state = g_strdup (r->state);
      c->finished_at = r->finished_at;
      g_ptr_array_add (out, c);
    }
  return out;
}

BroArchivedMatch *
bro_state_archived_match (BroState *self, BroDiscInfo *info)
{
  g_autofree char *fp = bro_disc_fingerprint (info);
  g_autoptr (GPtrArray) candidates = NULL;
  if (!fp)
    return NULL;
  candidates = bro_state_archived_candidates (self);
  return bro_find_archived (fp, candidates, NULL);
}

static char *
check_records_path (void)
{
  g_autofree char *dir = bro_data_dir ();
  return g_build_filename (dir, "archive-checks.json", NULL);
}

BroCheckRecord *
bro_state_check_record (BroState *self, const char *folder)
{
  return folder && self->check_records ? g_hash_table_lookup (self->check_records, folder) : NULL;
}

gboolean
bro_state_verify_due (BroState *self, gint64 now)
{
  g_autofree char *root = NULL;
  BroCheckRecord *last;
  int days = self->config->archive_check_interval_days;
  if (days <= 0 || self->verify.running)
    return FALSE;
  root = bro_state_output_root (self);
  if (!g_file_test (root, G_FILE_TEST_IS_DIR))
    return FALSE;
  last = bro_state_check_record (self, root);
  return !last || now - last->checked_at >= (gint64) days * 86400;
}

typedef struct {
  BroState *self;          /* ref */
  char *path;
  gboolean scheduled;
  GPtrArray *targets;      /* BroNotificationTarget* (a copy of the configuration's, for a scheduled check) */
  BroAppConfig *config;    /* owns targets */
  GCancellable *cancel;
  GMutex lock;             /* guards the progress below */
  gint64 done, total;
  char *folder, *file;
  GPtrArray *results;      /* set by the worker */
  gboolean stopped;
  guint poll;
} VerifyTask;

static void
verify_task_free (VerifyTask *t)
{
  g_clear_handle_id (&t->poll, g_source_remove);
  g_object_unref (t->self);
  g_free (t->path);
  if (t->config) bro_app_config_free (t->config);
  g_object_unref (t->cancel);
  g_mutex_clear (&t->lock);
  g_free (t->folder);
  g_free (t->file);
  if (t->results) g_ptr_array_unref (t->results);
  g_free (t);
}

static gboolean
verify_progress (gint64 done, gint64 total, const char *folder, const char *file, gpointer data)
{
  VerifyTask *t = data;
  g_mutex_lock (&t->lock);
  t->done = done;
  t->total = total;
  if (folder && g_strcmp0 (folder, t->folder) != 0)
    {
      g_free (t->folder);
      t->folder = g_strdup (folder);
    }
  if (file && g_strcmp0 (file, t->file) != 0)
    {
      g_free (t->file);
      t->file = g_strdup (file);
    }
  g_mutex_unlock (&t->lock);
  return !g_cancellable_is_cancelled (t->cancel);
}

static gboolean
verify_poll (gpointer data)
{
  VerifyTask *t = data;
  BroState *self = t->self;
  g_mutex_lock (&t->lock);
  self->verify.done = t->done;
  self->verify.total = t->total;
  g_free (self->verify.folder);
  self->verify.folder = g_strdup (t->folder);
  g_free (self->verify.file);
  self->verify.file = g_strdup (t->file);
  g_mutex_unlock (&t->lock);
  emit (self, STATE_VERIFY_CHANGED);
  return G_SOURCE_CONTINUE;
}

static int
count_damaged (GPtrArray *results)
{
  int n = 0;
  for (guint i = 0; results && i < results->len; i++)
    n += !bro_folder_check_ok (results->pdata[i]);
  return n;
}

/* "Archive check: all 12 folder(s) OK" and the damaged folders, for notifications. */
static void
verify_report (GPtrArray *results, char **title, char **body)
{
  int damaged = count_damaged (results);
  GString *b = g_string_new (NULL);
  *title = damaged ? g_strdup_printf ("Archive check: %d of %u folder(s) damaged", damaged, results->len)
                   : g_strdup_printf ("Archive check: all %u folder(s) OK", results->len);
  for (guint i = 0, shown = 0; i < results->len; i++)
    {
      BroFolderCheck *r = results->pdata[i];
      g_autofree char *summary = NULL;
      if (bro_folder_check_ok (r))
        continue;
      if (++shown > 10)
        {
          g_string_append_printf (b, "… and %d more\n", damaged - 10);
          break;
        }
      summary = bro_folder_check_summary (r);
      g_string_append_printf (b, "%s: %s\n", r->folder, summary);
    }
  if (!damaged)
    g_string_append (b, "Every file matches its checksum.");
  *body = g_string_free (b, FALSE);
}

static void
verify_log (const char *text, gpointer data)
{
  g_message ("%s", text);
}

static void
verify_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  VerifyTask *t = task_data;
  g_autoptr (GPtrArray) folders = bro_archive_folders (t->path);
  t->results = bro_verify_folders (folders, verify_progress, t, &t->stopped);
  if (t->scheduled && !t->stopped && t->targets && t->targets->len)
    {
      g_autofree char *title = NULL, *body = NULL;
      verify_report (t->results, &title, &body);
      bro_notifications_send (t->targets, title, body, count_damaged (t->results) ? "failed" : "success", verify_log, NULL);
    }
  g_task_return_boolean (task, TRUE);
}

static void
verify_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  VerifyTask *t = g_task_get_task_data (G_TASK (res));
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gboolean path_is_folder = FALSE;
  int damaged = count_damaged (t->results);
  g_autofree char *records = check_records_path ();
  self->verify_task = NULL;
  self->verify.running = FALSE;
  self->verify.stopped = t->stopped;
  self->verify.finished_at = now;
  if (self->verify.results)
    g_ptr_array_unref (self->verify.results);
  self->verify.results = g_ptr_array_ref (t->results);
  for (guint i = 0; i < t->results->len; i++)
    {
      BroFolderCheck *r = t->results->pdata[i];
      bro_check_records_add (self->check_records, r, now);
      path_is_folder |= g_str_equal (r->folder, t->path);
    }
  /* The whole tree, so a scheduled check knows when it last ran. */
  if (!t->stopped && !path_is_folder)
    {
      BroCheckRecord *rec = g_new0 (BroCheckRecord, 1);
      rec->checked_at = now;
      rec->ok = damaged == 0;
      rec->summary = damaged ? g_strdup_printf ("%d of %u archive folder(s) damaged", damaged, t->results->len)
                             : g_strdup_printf ("%u archive folder(s), all OK", t->results->len);
      g_hash_table_replace (self->check_records, g_strdup (t->path), rec);
    }
  bro_check_records_save (self->check_records, records);
  if (t->scheduled && damaged)
    {
      g_autofree char *title = NULL, *body = NULL;
      verify_report (t->results, &title, &body);
      bro_state_set_error (self, title);
      if (self->app)
        {
          g_autoptr (GNotification) n = g_notification_new (title);
          g_notification_set_body (n, body);
          g_application_send_notification (self->app, "archive-check", n);
        }
    }
  emit (self, STATE_VERIFY_CHANGED);
  emit (self, STATE_HISTORY_CHANGED); /* the history shows when folders were last checked */
  update_keep_awake (self);
}

gboolean
bro_state_verify_start (BroState *self, const char *path, gboolean scheduled)
{
  VerifyTask *t;
  GTask *task;
  if (self->verify.running || !path || !*path)
    return FALSE;
  t = g_new0 (VerifyTask, 1);
  t->self = g_object_ref (self);
  t->path = g_strdup (path);
  t->scheduled = scheduled;
  if (scheduled)
    {
      t->config = bro_app_config_copy (self->config);
      t->targets = t->config->notifications;
    }
  t->cancel = g_cancellable_new ();
  g_mutex_init (&t->lock);
  self->verify_task = t;
  self->verify.running = TRUE;
  self->verify.scheduled = scheduled;
  self->verify.stopped = FALSE;
  self->verify.done = self->verify.total = 0;
  self->verify.started_at = g_get_real_time () / G_USEC_PER_SEC;
  self->verify.finished_at = 0;
  g_free (self->verify.path);
  self->verify.path = g_strdup (path);
  g_clear_pointer (&self->verify.folder, g_free);
  g_clear_pointer (&self->verify.file, g_free);
  g_clear_pointer (&self->verify.results, g_ptr_array_unref);
  t->poll = g_timeout_add (250, verify_poll, t);
  task = g_task_new (self, NULL, verify_done, NULL);
  g_task_set_task_data (task, t, (GDestroyNotify) verify_task_free);
  g_task_run_in_thread (task, verify_thread);
  g_object_unref (task);
  emit (self, STATE_VERIFY_CHANGED);
  update_keep_awake (self);
  return TRUE;
}

void
bro_state_verify_cancel (BroState *self)
{
  VerifyTask *t = self->verify_task;
  if (t)
    g_cancellable_cancel (t->cancel);
}

/* ---- background post-processing ---- */

static void
background_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  BroBackgroundItem *b = task_data;
  int ran = 0;
  char *failed = bro_background_work_run (b->work, &ran, NULL);
  g_object_set_data (G_OBJECT (task), "ran", GINT_TO_POINTER (ran));
  g_task_return_pointer (task, failed, g_free);
}

static void
background_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  g_autofree char *id = data;
  int ran = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (res), "ran"));
  g_autofree char *failed = g_task_propagate_pointer (G_TASK (res), NULL);
  for (guint i = 0; i < self->background->len; i++)
    {
      BroBackgroundItem *b = self->background->pdata[i];
      if (!g_str_equal (b->id, id))
        continue;
      g_free (b->state);
      g_free (b->message);
      b->state = g_strdup (failed ? "failed" : "done");
      b->message = failed ? g_strdup_printf ("“%s” failed; see %s", failed, b->work->log_file)
                          : g_strdup_printf ("%d step(s) finished", ran);
    }
  emit (self, STATE_JOBS_CHANGED);
  pump_background (self);
}

static void
pump_background (BroState *self)
{
  int running = 0;
  for (guint i = 0; i < self->background->len; i++)
    running += g_str_equal (((BroBackgroundItem *) self->background->pdata[i])->state, "running");
  for (guint i = 0; i < self->background->len && running < MAX (1, self->config->background_jobs); i++)
    {
      BroBackgroundItem *b = self->background->pdata[i];
      GTask *task;
      if (!g_str_equal (b->state, "queued"))
        continue;
      g_free (b->state);
      b->state = g_strdup ("running");
      running++;
      task = g_task_new (self, NULL, background_done, g_strdup (b->id));
      /* The item stays in the list while it runs (it is only removed once finished). */
      g_task_set_task_data (task, b, NULL);
      g_task_run_in_thread (task, background_thread);
      g_object_unref (task);
    }
}

void
bro_state_enqueue_background (BroState *self, BroBackgroundWork *work)
{
  BroBackgroundItem *b = g_new0 (BroBackgroundItem, 1);
  b->id = g_uuid_string_random ();
  b->work = work;
  b->state = g_strdup ("queued");
  b->message = g_strdup ("");
  g_ptr_array_add (self->background, b);
  pump_background (self);
  emit (self, STATE_JOBS_CHANGED);
}

gboolean
bro_state_background_busy (BroState *self)
{
  for (guint i = 0; i < self->background->len; i++)
    {
      const char *st = ((BroBackgroundItem *) self->background->pdata[i])->state;
      if (g_str_equal (st, "queued") || g_str_equal (st, "running"))
        return TRUE;
    }
  return FALSE;
}

void
bro_state_clear_background (BroState *self)
{
  for (guint i = self->background->len; i > 0; i--)
    {
      const char *st = ((BroBackgroundItem *) self->background->pdata[i - 1])->state;
      if (g_str_equal (st, "done") || g_str_equal (st, "failed"))
        g_ptr_array_remove_index (self->background, i - 1);
    }
  emit (self, STATE_JOBS_CHANGED);
}

/* ---- trays ---- */

static void
close_tray_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  GPtrArray *devices = task_data;
  gboolean ok = TRUE;
  for (guint i = 0; i < devices->len; i++)
    ok &= bro_close_tray (devices->pdata[i]);
  g_task_return_boolean (task, ok);
}

static void
close_tray_done (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  if (!g_task_propagate_boolean (G_TASK (res), NULL))
    bro_state_set_error (self, "Could not close the tray (is the eject command installed?)");
  bro_state_schedule_rescan (self, 5);
}

static void
close_trays (BroState *self, GPtrArray *devices)
{
  GTask *task;
  if (!devices->len)
    {
      g_ptr_array_unref (devices);
      return;
    }
  task = g_task_new (self, NULL, close_tray_done, NULL);
  g_task_set_task_data (task, devices, (GDestroyNotify) g_ptr_array_unref);
  g_task_run_in_thread (task, close_tray_thread);
  g_object_unref (task);
}

void
bro_state_close_tray (BroState *self, const char *lane)
{
  BroDriveEntry *e = bro_state_entry_for_lane (self, lane);
  GPtrArray *devices = g_ptr_array_new_with_free_func (g_free);
  if (e && e->device && *e->device)
    g_ptr_array_add (devices, g_strdup (e->device));
  close_trays (self, devices);
}

void
bro_state_close_all_trays (BroState *self)
{
  GPtrArray *devices = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < self->drives->len; i++)
    {
      BroDriveEntry *e = self->drives->pdata[i];
      if (bro_drive_entry_present (e) && e->state != BRO_DRIVE_INSERTED && e->device && *e->device)
        g_ptr_array_add (devices, g_strdup (e->device));
    }
  close_trays (self, devices);
}

/* ---- beta key updates ---- */

typedef struct {
  char *installed;
  char *reason;
} BetaCheck;

static void
beta_check_free (BetaCheck *b)
{
  g_free (b->installed);
  g_free (b->reason);
  g_free (b);
}

static void
beta_check_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
  BetaCheck *b = task_data;
  g_autofree char *key = bro_beta_key_fetch (NULL);
  g_task_return_boolean (task, key && g_strcmp0 (key, b->installed) != 0);
}

static void
beta_installed (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  g_autofree char *reason = data;
  g_autofree char *result = bro_state_install_beta_key_finish (self, res);
  g_autofree char *msg = g_strdup_printf ("Beta key updated %s: %s", reason, result ? result : "");
  bro_state_set_error (self, msg);
}

static void
beta_checked (GObject *src, GAsyncResult *res, gpointer data)
{
  BroState *self = BRO_STATE (src);
  BetaCheck *b = g_task_get_task_data (G_TASK (res));
  if (g_task_propagate_boolean (G_TASK (res), NULL))
    bro_state_install_beta_key (self, beta_installed, g_strdup (b->reason));
}

void
bro_state_update_beta_key_if_needed (BroState *self, const char *reason)
{
  BetaCheck *b;
  GTask *task;
  const char *installed = self->config->registration_key;
  g_autoptr (GHashTable) settings = NULL;
  if (!installed || !*installed)
    {
      settings = bro_makemkv_installed_settings ();
      installed = g_hash_table_lookup (settings, "app_Key");
    }
  if (!bro_beta_key_may_replace (installed))
    return;
  b = g_new0 (BetaCheck, 1);
  b->installed = g_strdup (installed ? installed : "");
  b->reason = g_strdup (reason);
  task = g_task_new (self, NULL, beta_checked, NULL);
  g_task_set_task_data (task, b, (GDestroyNotify) beta_check_free);
  g_task_run_in_thread (task, beta_check_thread);
  g_object_unref (task);
}

/* ---- web page ---- */

static const char *
web_state (BroJobState s)
{
  switch (s)
    {
    case BRO_JOB_QUEUED: return "queued";
    case BRO_JOB_WAITING: return "waiting";
    case BRO_JOB_RUNNING: return "running";
    default: return bro_job_state_word (s);
    }
}

static const char *
word_label (const char *word)
{
  for (int s = BRO_JOB_SUCCEEDED; s <= BRO_JOB_COMPLETED_WITH_ERRORS; s++)
    if (g_strcmp0 (bro_job_state_word (s), word) == 0)
      return bro_job_state_label (s);
  return word ? word : "";
}

#define JS(k, v) do { json_builder_set_member_name (b, k); json_builder_add_string_value (b, (v) ? (v) : ""); } while (0)

JsonNode *
bro_state_web_status (BroState *self)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GPtrArray) items = bro_state_drive_items (self);
  json_builder_begin_object (b);
  JS ("app", "Bromelia");
  JS ("version", PACKAGE_VERSION);
  JS ("makemkv", self->makemkv_version);
  JS ("problem", self->makemkv_problem != BRO_NOTICE_NONE ? bro_notice_explanation (self->makemkv_problem) : "");
  json_builder_set_member_name (b, "drives");
  json_builder_begin_array (b);
  for (guint i = 0; i < items->len; i++)
    {
      BroDriveItem *d = items->pdata[i];
      g_autofree char *name = NULL;
      if (!d->entry)
        continue;
      name = bro_drive_item_name (d);
      json_builder_begin_object (b);
      JS ("lane", d->lane);
      JS ("name", name);
      JS ("device", d->entry->device);
      JS ("state", bro_drive_state_name (d->entry->state));
      json_builder_set_member_name (b, "hasDisc");
      json_builder_add_boolean_value (b, d->entry->state == BRO_DRIVE_INSERTED);
      JS ("disc", d->entry->disc_name);
      json_builder_set_member_name (b, "busy");
      json_builder_add_boolean_value (b, bro_state_active_job (self, d->lane) != NULL);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_set_member_name (b, "jobs");
  json_builder_begin_array (b);
  for (guint i = 0; i < self->jobs->len; i++)
    {
      BroJob *j = self->jobs->pdata[i];
      g_autofree char *title = bro_job_title (j);
      json_builder_begin_object (b);
      JS ("id", j->id);
      JS ("title", title);
      JS ("state", web_state (j->state));
      JS ("stateLabel", bro_job_state_label (j->state));
      JS ("phase", j->phase);
      json_builder_set_member_name (b, "progress");
      json_builder_add_double_value (b, bro_job_overall (j));
      JS ("error", j->error);
      JS ("outputDirectory", j->output_dir);
      JS ("lane", j->lane);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_set_member_name (b, "background");
  json_builder_begin_array (b);
  for (guint i = 0; i < self->background->len; i++)
    {
      BroBackgroundItem *bg = self->background->pdata[i];
      json_builder_begin_object (b);
      JS ("id", bg->id);
      JS ("title", bg->work->title);
      JS ("state", bg->state);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_set_member_name (b, "history");
  json_builder_begin_array (b);
  for (guint i = 0; i < self->history->len && i < 20; i++)
    {
      BroHistoryRecord *h = self->history->pdata[i];
      g_autoptr (GDateTime) t = h->finished_at ? g_date_time_new_from_unix_utc (h->finished_at) : NULL;
      g_autofree char *when = t ? g_date_time_format_iso8601 (t) : g_strdup ("");
      json_builder_begin_object (b);
      JS ("title", h->title);
      JS ("state", h->state);
      JS ("stateLabel", word_label (h->state));
      JS ("error", h->error);
      JS ("outputDirectory", h->output_dir);
      JS ("finishedAt", when);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_set_member_name (b, "verify");
  json_builder_begin_object (b);
  {
    BroVerifyStatus *v = &self->verify;
    g_autoptr (GDateTime) t = v->finished_at ? g_date_time_new_from_unix_utc (v->finished_at) : NULL;
    g_autofree char *when = t ? g_date_time_format_iso8601 (t) : g_strdup ("");
    json_builder_set_member_name (b, "running");
    json_builder_add_boolean_value (b, v->running);
    JS ("path", v->path);
    json_builder_set_member_name (b, "progress");
    json_builder_add_double_value (b, v->total > 0 ? (double) v->done / v->total : 0);
    JS ("folder", v->folder);
    json_builder_set_member_name (b, "stopped");
    json_builder_add_boolean_value (b, v->stopped);
    JS ("finishedAt", when);
    json_builder_set_member_name (b, "folders");
    json_builder_add_int_value (b, v->results ? v->results->len : 0);
    json_builder_set_member_name (b, "damaged");
    json_builder_begin_array (b);
    for (guint i = 0; v->results && i < v->results->len; i++)
      {
        BroFolderCheck *r = v->results->pdata[i];
        g_autofree char *summary = NULL;
        if (bro_folder_check_ok (r))
          continue;
        summary = bro_folder_check_summary (r);
        json_builder_begin_object (b);
        JS ("folder", r->folder);
        JS ("summary", summary);
        json_builder_end_object (b);
      }
    json_builder_end_array (b);
  }
  json_builder_end_object (b);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

#undef JS

char *
bro_state_web_action (BroState *self, const char *kind, const char *id, const char *action)
{
  if (g_str_equal (kind, "drives") && (g_str_equal (action, "rip") || g_str_equal (action, "eject") || g_str_equal (action, "close")))
    {
      g_autoptr (GPtrArray) items = bro_state_drive_items (self);
      BroDriveItem *item = NULL;
      for (guint i = 0; i < items->len; i++)
        if (g_str_equal (((BroDriveItem *) items->pdata[i])->lane, id) && ((BroDriveItem *) items->pdata[i])->entry)
          item = items->pdata[i];
      if (!item)
        return g_strdup ("No such drive");
      if (g_str_equal (action, "rip"))
        {
          guint before = self->jobs->len;
          if (bro_state_active_job (self, id))
            return g_strdup ("The drive is busy");
          g_clear_pointer (&self->last_error, g_free);
          bro_state_quick_rip (self, item, -1);
          if (self->jobs->len > before)
            return NULL;
          return g_strdup (self->last_error ? self->last_error : "Nothing to rip");
        }
      if (g_str_equal (action, "eject"))
        bro_state_eject (self, id);
      else
        bro_state_close_tray (self, id);
      return NULL;
    }
  if (g_str_equal (kind, "jobs") && g_str_equal (action, "cancel"))
    {
      for (guint i = 0; i < self->jobs->len; i++)
        {
          BroJob *j = self->jobs->pdata[i];
          if (g_str_equal (j->id, id))
            {
              bro_state_cancel_job (self, j);
              return NULL;
            }
        }
      return g_strdup ("No such job");
    }
  if (g_str_equal (kind, "verify"))
    {
      g_autofree char *root = bro_state_output_root (self);
      if (g_str_equal (action, "cancel"))
        {
          bro_state_verify_cancel (self);
          return NULL;
        }
      if (!g_str_equal (action, "start"))
        return g_strdup ("Unknown action");
      if (self->verify.running)
        return g_strdup ("A check is already running");
      if (!g_file_test (root, G_FILE_TEST_IS_DIR))
        return g_strdup_printf ("The output folder %s doesn't exist", root);
      bro_state_verify_start (self, root, FALSE);
      return NULL;
    }
  return g_strdup ("Unknown action");
}

const char *
bro_state_web_error (BroState *self)
{
  return bro_web_server_last_error (self->web);
}
