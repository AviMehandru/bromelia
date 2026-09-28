/* bro-runner.h — the job pipeline (scan → select → rip / backup → remux → rename → post-process → eject).
 *
 * bro_run_job() is synchronous and meant to run on a worker thread. Progress is reported through the
 * request's on_update callback (called on the worker thread); the caller marshals it to the UI.
 */
#pragma once

#include <gio/gio.h>
#include "bro-config.h"
#include "bro-makemkv.h"
#include "bro-robot.h"

G_BEGIN_DECLS

typedef enum {
  BRO_JOB_QUEUED,
  BRO_JOB_WAITING,
  BRO_JOB_RUNNING,
  BRO_JOB_SUCCEEDED,
  BRO_JOB_FAILED,
  BRO_JOB_CANCELLED,
  /* Every title was saved, but MakeMKV reported read errors while ripping, so the files may contain damaged or
   * skipped data. They are kept apart from finished archives. */
  BRO_JOB_COMPLETED_WITH_ERRORS,
} BroJobState;

const char *bro_job_state_label (BroJobState s);
const char *bro_job_state_word (BroJobState s);
gboolean    bro_job_state_finished (BroJobState s);

typedef enum {
  BRO_UPDATE_LOG,
  BRO_UPDATE_PHASE,
  BRO_UPDATE_OPERATION,
  BRO_UPDATE_TOTAL_OPERATION,
  BRO_UPDATE_PROGRESS,
  BRO_UPDATE_STEPS,
  BRO_UPDATE_DISC_LABEL,
  BRO_UPDATE_OUTPUT_DIR,
  BRO_UPDATE_FILE,
  BRO_UPDATE_COMMAND,
} BroUpdateKind;

typedef struct {
  BroUpdateKind kind;
  BroSeverity severity;
  char *text;
  double current, total;
  int step_index, step_count;
} BroUpdate;

typedef void (*BroUpdateFunc) (const BroUpdate *update, gpointer user_data);

typedef struct {
  char *job_id;
  char *job_dir;
  BroAppConfig *config;          /* owned */
  BroDriveConfig *drive;         /* owned */
  BroSource *source;             /* owned */
  char *disc_label;
  BroRipMode mode;
  BroDiscInfo *preloaded;        /* owned reference or NULL */
  GArray *manual_titles;         /* int, owned, or NULL for the drive's rules */
  GHashTable *track_selections;  /* int -> GHashTable* (int set), owned */
  GHashTable *name_overrides;    /* int -> char*, owned */
  char *media_name;              /* movie / show name; "" = inferred from the disc */
  int media_kind;                /* BroMediaKind, -1 = inferred */
  int first_episode;             /* -1 = read from the menus, or 1 */
  int disc_flags;                /* DRV flags, -1 = unknown */
  char *makemkvcon;
  char *mkvmerge;                /* may be NULL */
  gboolean skip_eject;
  GCancellable *cancellable;     /* owned */
  BroUpdateFunc on_update;
  gpointer user_data;
} BroRunRequest;

typedef struct {
  BroJobState status;
  char *error;
  char *output_dir;
  GPtrArray *files;
  char *disc_label;
  BroDiscInfo *info;
  int warnings, errors;
  gint64 started_at, finished_at; /* unix seconds */
} BroRunResult;

BroRunRequest *bro_run_request_new (void);
void           bro_run_request_free (BroRunRequest *r);
void           bro_run_result_free (BroRunResult *r);
BroRunResult  *bro_run_job (BroRunRequest *req);

GHashTable *bro_int_set_new (void);
GHashTable *bro_track_selections_new (void);

/* Exposed for unit tests. */
GPtrArray *bro_remux_arguments (const char *mkvmerge, GPtrArray *layout_types, BroTitle *title, GHashTable *keep,
                                const char *input, const char *output);
GPtrArray *bro_post_step_argv (const BroPostStep *step, GHashTable *values, GPtrArray *files);
gboolean   bro_post_step_should_run (const BroPostStep *step, BroJobState status);
char      *bro_unique_path (const char *path);

/* Name prefix of the hidden staging folder a job writes to inside its output folder. */
#define BRO_STAGING_PREFIX ".bromelia-incomplete-"

/* A ripped MKV as reported by `mkvmerge -J`. */
typedef struct {
  gboolean has_duration;
  double duration;        /* seconds */
  GPtrArray *track_types; /* char*: "video", "audio", "subtitles", ... */
  int chapters;
} BroMkvProbe;

BroMkvProbe *bro_mkv_probe_parse (const char *json); /* NULL when mkvmerge did not recognise the file */
void         bro_mkv_probe_free (BroMkvProbe *p);
/* Checks a ripped MKV against its title in the disc listing. Adds reasons the file can't be trusted to problems
 * and differences worth noting to notes (both char*). */
void         bro_rip_check (const BroMkvProbe *p, BroTitle *title, GPtrArray *problems, GPtrArray *notes);
double       bro_rip_duration_tolerance (double expected);
/* Why a backup doesn't look like a disc (BDMV / VIDEO_TS / HVDVD_TS folder, or ISO 9660 / UDF image), or NULL. */
char        *bro_backup_problem (const char *path, gboolean iso);
/* Why `now` looks like a different disc than `old` (the listing it was opened with), or NULL. */
char        *bro_listing_different_disc (BroDiscInfo *old, BroDiscInfo *now);
/* Maps titles of `old` (indices) to titles of `now` by source title, duration and segment map. Returns
 * int -> (index + 1), or NULL with error when a title is missing or, for titles in same_tracks (int set, may be
 * NULL), when its track list changed. */
GHashTable  *bro_listing_map (GArray *indices, BroDiscInfo *old, BroDiscInfo *now, GHashTable *same_tracks, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMkvProbe, bro_mkv_probe_free)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunRequest, bro_run_request_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunResult, bro_run_result_free)

G_END_DECLS
