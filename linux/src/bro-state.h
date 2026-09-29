/* bro-state.h — application state: configuration, drives, disc sessions, job queue and history.
 * All objects here live on the main thread; work runs on worker threads and reports back through
 * the main context. */
#pragma once

#include <gio/gio.h>
#include "bro-config.h"
#include "bro-makemkv.h"
#include "bro-runner.h"
#include "bro-sleep.h"
#include "bro-web.h"

G_BEGIN_DECLS

typedef struct {
  gint64 time;
  BroSeverity severity;
  char *text;
} BroLogEntry;

void bro_log_entry_free (BroLogEntry *e);

/* ---- job ---- */

#define BRO_TYPE_JOB (bro_job_get_type ())
G_DECLARE_FINAL_TYPE (BroJob, bro_job, BRO, JOB, GObject)

struct _BroJob {
  GObject parent_instance;
  char *id;
  char *lane;
  char *source_label;
  char *disc_label;
  BroSource *source;
  BroDriveConfig *drive;       /* snapshot */
  BroRipMode mode;
  BroDiscInfo *preloaded;
  GArray *manual_titles;       /* int or NULL */
  GHashTable *track_selections;
  GHashTable *name_overrides;
  char *media_name;            /* "" = inferred */
  int media_kind;              /* BroMediaKind, -1 = inferred */
  int first_episode;           /* -1 = menus / 1 */
  int disc_flags;              /* DRV flags, -1 = unknown */
  gboolean automatic;
  gboolean uses_configured_mode; /* the drive's mode, so it may follow the disc's format (automatic and quick rips) */
  gint64 start_at;             /* unix seconds; 0 = start when possible */

  BroJobState state;
  char *phase, *operation, *total_operation;
  double current, total;
  int step_index, step_count;
  gint64 started_at, finished_at;
  char *output_dir;
  GPtrArray *files;
  char *error;
  int warnings, errors;
  GPtrArray *log;              /* BroLogEntry* */
  GPtrArray *commands;
  GCancellable *cancellable;
  gint64 worker_last_progress; /* touched only by the worker thread */
};

double bro_job_overall (BroJob *job);
char  *bro_job_title (BroJob *job);
gint64 bro_job_elapsed (BroJob *job);
gint64 bro_job_remaining (BroJob *job); /* -1 when unknown */
char  *bro_job_log_path (BroJob *job);
char  *bro_job_dir (BroJob *job);

/* ---- disc session ---- */

#define BRO_TYPE_SESSION (bro_session_get_type ())
G_DECLARE_FINAL_TYPE (BroSession, bro_session, BRO, SESSION, GObject)

struct _BroSession {
  GObject parent_instance;
  char *id;
  BroSource *source;
  char *config_id;
  BroDiscInfo *info;
  gboolean loading;
  char *error;
  double progress;
  char *operation;
  GPtrArray *log;               /* BroLogEntry* */
  GHashTable *selected;         /* int set */
  GHashTable *track_selections; /* int -> int set; only customised titles */
  GHashTable *name_overrides;   /* int -> char* */
  char *output_override;
  char *media_name;             /* movie / show name for file names; "" = inferred */
  int media_kind;               /* BroMediaKind, -1 = inferred */
  int first_episode;            /* -1 = read from the menus, or 1 */
  int disc_flags;               /* DRV flags, -1 = unknown */
  GCancellable *cancellable;
  char *libre_drive;            /* what MakeMKV said about LibreDrive when the disc was opened (drives only), or NULL */
  gboolean libre_drive_required;
};

void     bro_session_reset (BroSession *s);
void     bro_session_apply_rule (BroSession *s, const BroTitleSelection *rule);
void     bro_session_set_selected (BroSession *s, int title, gboolean selected);
void     bro_session_select_all (BroSession *s, gboolean all);
gboolean bro_session_has_custom_tracks (BroSession *s, int title);
void     bro_session_customize_tracks (BroSession *s, int title);
void     bro_session_reset_tracks (BroSession *s, int title);
gboolean bro_session_track_selected (BroSession *s, int title, int track);
void     bro_session_set_track (BroSession *s, int title, int track, gboolean selected);
gint64   bro_session_selected_size (BroSession *s);

/* ---- state ---- */

typedef struct {
  char *id;
  char *lane;
  BroDriveEntry *entry;    /* NULL when disconnected */
  BroDriveConfig *config;  /* borrowed; NULL when not configured */
} BroDriveItem;

void  bro_drive_item_free (BroDriveItem *item);
char *bro_drive_item_name (BroDriveItem *item);

/* Post-processing steps marked "background" (encoding, uploads), run after their job, a few at a time. */
typedef struct {
  char *id;
  BroBackgroundWork *work;
  char *state;   /* queued, running, done, failed */
  char *message;
} BroBackgroundItem;

typedef struct {
  char *id, *title, *drive_name, *disc_name, *mode, *state, *output_dir, *error, *log_path;
  gint64 started_at, finished_at;
  GPtrArray *files;
  int warnings, errors;
} BroHistoryRecord;

#define BRO_TYPE_STATE (bro_state_get_type ())
G_DECLARE_FINAL_TYPE (BroState, bro_state, BRO, STATE, GObject)

struct _BroState {
  GObject parent_instance;
  GApplication *app;
  BroAppConfig *config;
  char *config_path;
  GPtrArray *drives;         /* BroDriveEntry* (present only) */
  GHashTable *known_states;  /* lane -> state */
  gboolean first_scan_done;
  GHashTable *sessions;      /* id -> BroSession* */
  GPtrArray *file_sessions;  /* BroSession* (refs) */
  GPtrArray *jobs;           /* BroJob* (refs) */
  GPtrArray *history;        /* BroHistoryRecord* */
  gboolean scanning;
  char *makemkv_version;
  char *last_error;
  gint64 last_scan;
  GPtrArray *scan_messages;  /* char* */
  int makemkv_problem;       /* BroNotice: expired key, outdated MakeMKV…, from the last makemkvcon run */
  guint save_source, tick_source, poll_source, rescan_source;
  GVolumeMonitor *monitor;
  GPtrArray *background;     /* BroBackgroundItem* */
  BroWebServer *web;
  gboolean beta_key_tried;   /* the automatic update after an expired key runs once per session */
  BroSleepInhibitor *inhibitor; /* owned; NULL = none */
  gboolean keeping_awake;
  char *unfinished_saved;    /* what unfinished-<pid>.json holds, to write it only when it changes */
};

BroState       *bro_state_new (GApplication *app);
void            bro_state_start (BroState *self);
void            bro_state_config_changed (BroState *self);
void            bro_state_save_now (BroState *self);
char           *bro_state_makemkvcon (BroState *self);
char           *bro_state_mkvmerge (BroState *self);
void            bro_state_set_error (BroState *self, const char *error);

void            bro_state_refresh_drives (BroState *self, gboolean force);
void            bro_state_apply_scan (BroState *self, GPtrArray *entries); /* BroDriveEntry* */
void            bro_state_schedule_rescan (BroState *self, guint seconds);
GPtrArray      *bro_state_drive_items (BroState *self); /* BroDriveItem* */
BroDriveItem   *bro_state_drive_item (BroState *self, const char *id);
BroDriveEntry  *bro_state_entry_for_lane (BroState *self, const char *lane);
BroDriveConfig *bro_state_configure_drive (BroState *self, const BroDriveEntry *entry);
void            bro_state_remove_drive_config (BroState *self, const char *id);
void            bro_state_eject (BroState *self, const char *lane);

BroSession     *bro_state_session (BroState *self, const char *id);
BroDriveConfig *bro_state_session_config (BroState *self, BroSession *s);
BroSession     *bro_state_open_file (BroState *self, const char *path);
/* Downloads the current beta key from the MakeMKV forum and registers it; the result is a message for the user. */
void            bro_state_install_beta_key (BroState *self, GAsyncReadyCallback cb, gpointer data);
char           *bro_state_install_beta_key_finish (BroState *self, GAsyncResult *res);
void            bro_state_clear_problem (BroState *self);
void            bro_state_close_file (BroState *self, BroSession *s);
void            bro_state_load_disc (BroState *self, BroSession *s);

BroJob         *bro_state_active_job (BroState *self, const char *lane);
BroJob         *bro_state_recent_job (BroState *self, const char *lane);
void            bro_state_quick_rip (BroState *self, BroDriveItem *item, int mode /* -1 = configured */);
void            bro_state_rip_session (BroState *self, BroSession *s, BroRipMode mode);
void            bro_state_cancel_job (BroState *self, BroJob *job);
void            bro_state_start_now (BroState *self, BroJob *job);
void            bro_state_retry (BroState *self, BroJob *job);
void            bro_state_move_job (BroState *self, BroJob *job, int offset);
void            bro_state_clear_finished (BroState *self);
int             bro_state_active_count (BroState *self);
void            bro_state_clear_history (BroState *self);

void            bro_state_import_settings (BroState *self, GHashTable *settings);
void            bro_state_save_preset (BroState *self, const char *name, const BroDriveConfig *from);
void            bro_state_register_key (BroState *self, const char *key, GAsyncReadyCallback cb, gpointer data);
char           *bro_state_register_key_finish (BroState *self, GAsyncResult *res);

/* Keeping the computer awake: wanted while jobs or background steps run and preventSleep is on. The state turns the
 * inhibitor on and off as that changes (takes inhibitor; NULL = none). A headless state starts with logind's. */
gboolean        bro_state_wants_awake (BroState *self);
void            bro_state_set_sleep_inhibitor (BroState *self, BroSleepInhibitor *inhibitor);

/* Background post-processing (signal "jobs-changed" when it changes). */
void            bro_state_enqueue_background (BroState *self, BroBackgroundWork *work); /* takes work */
gboolean        bro_state_background_busy (BroState *self);
void            bro_state_clear_background (BroState *self);

void            bro_state_close_tray (BroState *self, const char *lane);
void            bro_state_close_all_trays (BroState *self);
/* With autoUpdateBetaKey: registers the forum's current beta key when MakeMKV uses a beta key (or none) and it
 * differs. A purchased key is never replaced. */
void            bro_state_update_beta_key_if_needed (BroState *self, const char *reason);

/* The web page: the JSON for /api/status, and actions (drives/<lane>/rip|eject|close, jobs/<id>/cancel; returns an
 * error or NULL). */
JsonNode       *bro_state_web_status (BroState *self);
char           *bro_state_web_action (BroState *self, const char *kind, const char *id, const char *action);
const char     *bro_state_web_error (BroState *self);
/* A state for a headless process (no GApplication), with its configuration at config_path (NULL = the default). */
BroState       *bro_state_new_headless (const char *config_path);

char           *bro_data_dir (void);

/* Queued, waiting and running jobs are also kept in <data dir>/unfinished-<pid>.json. At start, jobs left there by a
 * process that has gone (a crash, a power cut, a quit) are recorded in the history as interrupted (running) or not
 * started (queued, waiting); a running job's staging folder is made visible as "INCOMPLETE - <id>" with a note.
 * Returns how many were recovered. Called by bro_state_new*; public for the tests. */
int             bro_state_recover_unfinished (BroState *self);

G_END_DECLS
