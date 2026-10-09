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
  int first_episode;             /* -1 = read from the menus, continued from the previous disc of the set, or 1 */
  int media_year;                /* release year typed for this disc, to find the right movie or show online; 0 = none */
  char *online_id;               /* the movie or show chosen online (a TMDb or IMDb id); "" = the best search result */
  int disc_flags;                /* DRV flags, -1 = unknown */
  char *makemkvcon;
  char *mkvmerge;                /* may be NULL */
  gboolean skip_eject;
  gboolean automatic;            /* started on insert: wait for the disc to be mounted first */
  gboolean uses_configured_mode; /* the drive's mode (automatic and quick rips), so it may follow the disc's format */
  GCancellable *cancellable;     /* owned */
  GPtrArray *archived;           /* BroArchivedCandidate* from the history (owned), for "already archived" */
  BroUpdateFunc on_update;
  gpointer user_data;
} BroRunRequest;

/* Post-processing steps marked "background", to run after the job has finished (and the disc is out). */
typedef struct {
  char *job_id;
  char *title;
  GPtrArray *steps;   /* BroPostStep* (copies) */
  GHashTable *values; /* template values */
  char **environ;     /* BROMELIA_* variables */
  GPtrArray *files;   /* char* */
  char *output_dir;
  char *log_file;
  BroJobState status;
} BroBackgroundWork;

void  bro_background_work_free (BroBackgroundWork *w);
/* Runs the steps (blocking; call on a worker thread), logging to log_file. Returns the name of the first step that
 * failed, or NULL; *ran (optional) receives the number of steps run. */
char *bro_background_work_run (BroBackgroundWork *w, int *ran, GCancellable *cancellable);

typedef struct {
  BroJobState status;
  char *error;
  char *output_dir;
  GPtrArray *files;
  char *disc_label;
  BroDiscInfo *info;
  int warnings, errors;
  gint64 started_at, finished_at; /* unix seconds */
  char *libre_drive;              /* "Using LibreDrive mode (…)" details, or NULL */
  int problem;                    /* BroNotice: a problem with MakeMKV or the drive, BRO_NOTICE_NONE if none */
  BroRipMode mode;                /* the mode the job ran with (may follow the disc's format) */
  char *name;                     /* the movie / show name, or NULL */
  BroBackgroundWork *background;  /* steps for the background queue, or NULL */
  char *fingerprint;              /* the disc's fingerprint (bro_disc_fingerprint), or NULL */
} BroRunResult;

BroRunRequest *bro_run_request_new (void);
void           bro_run_request_free (BroRunRequest *r);
void           bro_run_result_free (BroRunResult *r);
BroRunResult  *bro_run_job (BroRunRequest *req);
/* Title and text of the notification for a finished job (what: the movie / show or disc). */
void           bro_notification_text (BroJobState status, BroRipMode mode, const char *what, guint file_count,
                                      const char *output_dir, const char *error, char **title, char **body);

GHashTable *bro_int_set_new (void);
GHashTable *bro_track_selections_new (void);

/* Exposed for unit tests. */
GPtrArray *bro_remux_arguments (const char *mkvmerge, GPtrArray *layout_types, BroTitle *title, GHashTable *keep,
                                const char *input, const char *output);
GPtrArray *bro_post_step_argv (const BroPostStep *step, GHashTable *values, GPtrArray *files);
gboolean   bro_post_step_should_run (const BroPostStep *step, BroJobState status);

/* HandBrake steps: HandBrakeCLI with a preset, one encode per ripped MKV, next to the archive (never over it). */
extern const char *const bro_handbrake_presets[]; /* presets built into HandBrake 1.6 and later, NULL terminated */
char      *bro_handbrake_tool (const BroPostStep *step);        /* the step's HandBrakeCLI, or the one installed; NULL */
gboolean   bro_handbrake_is_source (const char *file);          /* MKV files only (not backups or disc images) */
char      *bro_handbrake_output (const BroPostStep *step, GHashTable *values); /* the encode's path for one file */
/* [--preset-import-file F] --preset P -i input -o output [extra arguments] (without the tool). */
GPtrArray *bro_handbrake_arguments (const BroPostStep *step, const char *input, const char *output);
/* Keeps HandBrakeCLI's progress lines (Encoding: task 1 of 1, 45.12 %) at every 10 % of each task, and other lines. */
typedef struct {
  int task, last;
} BroProgressFilter;
gboolean   bro_handbrake_keep_line (BroProgressFilter *f, const char *line);
char      *bro_unique_path (const char *path);
/* A free name for something another program will create (MakeMKV makes a backup's destination itself, so nothing holds
 * the name until it has): "<stem><ext>" in @dir, else "<stem> (2)<ext>" and so on, held meanwhile by a hidden marker
 * next to it (".<name>.bromelia", created exclusively), so that two backups started together can't pick the same one.
 * @marker is set to the marker (NULL when the folder can't take one), which the caller removes once the thing has its
 * name. */
char      *bro_reserve_unique_path (const char *dir, const char *stem, const char *ext, char **marker);

/* Name prefix of the hidden staging folder a job writes to inside its output folder. */
#define BRO_STAGING_PREFIX ".bromelia-incomplete-"

/* Files of the job folder: everything makemkvcon printed, and MakeMKV's debug log (MakeMKV_log.txt) after each run. */
#define BRO_MAKEMKV_LOG       "makemkv.txt"
#define BRO_MAKEMKV_DEBUG_LOG "makemkv-debug.txt"

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
/* Ripping chosen titles in one makemkvcon run: the minimum length (seconds) that keeps exactly the chosen titles
 * (indices of info), or -1 when that isn't possible or worthwhile (fewer than three titles, every title, a title left out
 * that is about as long as a chosen one, or not above current). */
int          bro_one_pass_min_length (GArray *indices, BroDiscInfo *info, int current);
/* Whether listing (read with that minimum length) holds exactly the chosen titles of info. */
gboolean     bro_one_pass_matches (BroDiscInfo *listing, GArray *indices, BroDiscInfo *info);

/* Bytes free on the volume holding path (or its nearest existing parent), or -1 when unknown. */
gint64       bro_disk_available (const char *path);
/* bytes plus a margin (2 % or 256 MB, whichever is larger). */
gint64       bro_disk_required (gint64 bytes);
/* Why `now` looks like a different disc than `old` (the listing it was opened with), or NULL. */
char        *bro_listing_different_disc (BroDiscInfo *old, BroDiscInfo *now);
/* Maps titles of `old` (indices) to titles of `now` by source title, duration and segment map. Returns
 * int -> (index + 1), or NULL with error when a title is missing or, for titles in same_tracks (int set, may be
 * NULL), when its track list changed. */
GHashTable  *bro_listing_map (GArray *indices, BroDiscInfo *old, BroDiscInfo *now, GHashTable *same_tracks, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMkvProbe, bro_mkv_probe_free)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroBackgroundWork, bro_background_work_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunRequest, bro_run_request_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunResult, bro_run_result_free)

G_END_DECLS
