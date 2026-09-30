/* bro-config.h — configuration model (same JSON schema as the macOS and Windows versions). */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>
#include "bro-robot.h"

G_BEGIN_DECLS

typedef enum { BRO_STRATEGY_ALL, BRO_STRATEGY_LONGEST, BRO_STRATEGY_INDICES, BRO_STRATEGY_MANUAL } BroStrategy;
typedef enum { BRO_INDEX_MAKEMKV, BRO_INDEX_SOURCE } BroIndexBase;
/* AUDIO_CD and DATA_IMAGE are for discs without a DVD / Blu-ray structure (see BroOtherDiscs). */
typedef enum { BRO_MODE_MKV, BRO_MODE_BACKUP, BRO_MODE_BACKUP_DECRYPTED, BRO_MODE_BACKUP_THEN_MKV, BRO_MODE_INFO_ONLY,
               BRO_MODE_AUDIO_CD, BRO_MODE_DATA_IMAGE } BroRipMode;
#define BRO_VIDEO_MODE_COUNT 5 /* the modes a drive can be set to; the others are chosen from the kind of disc */
typedef enum { BRO_BACKUP_FOLDER, BRO_BACKUP_ISO } BroBackupFormat;
typedef enum { BRO_CONFLICT_UNIQUE, BRO_CONFLICT_OVERWRITE, BRO_CONFLICT_SKIP } BroConflictPolicy;
typedef enum { BRO_RUN_SUCCESS, BRO_RUN_FAILURE, BRO_RUN_ALWAYS } BroRunCondition;
typedef enum { BRO_PROFILE_MAKEMKV_DEFAULT, BRO_PROFILE_GENERATED, BRO_PROFILE_CUSTOM_FILE } BroProfileMode;
/* How output is named: with the folder and file name templates, or the way Plex / Jellyfin / Emby expect it. */
typedef enum { BRO_LAYOUT_TEMPLATES, BRO_LAYOUT_MEDIA_SERVER } BroLibraryLayout;
/* What an automatic rip does with a disc that has been archived before (manual rips only warn). */
typedef enum { BRO_ARCHIVED_SKIP, BRO_ARCHIVED_ASK, BRO_ARCHIVED_RIP_AGAIN } BroAlreadyArchived;
typedef enum { BRO_METADATA_NONE, BRO_METADATA_TMDB, BRO_METADATA_OMDB } BroMetadataProvider;
/* Disc formats with a mode of their own (rip.formatModes). */
typedef enum { BRO_FORMAT_KEY_DVD, BRO_FORMAT_KEY_BLURAY, BRO_FORMAT_KEY_UHD, BRO_FORMAT_KEY_COUNT } BroFormatKey;
typedef enum { BRO_LPCM_COPY, BRO_LPCM_LPCM, BRO_LPCM_WAVEX, BRO_LPCM_FLAC_BEST, BRO_LPCM_FLAC_FAST } BroLpcmOutput;

typedef struct {
  BroStrategy strategy;
  int longest_count;
  char *index_pattern;
  BroIndexBase index_base;
  int min_duration_seconds, max_duration_seconds;
  int min_chapters, max_chapters;
  int min_size_mb, max_size_mb;
  char *include_pattern;
  char *exclude_pattern;
  gboolean skip_duplicates;
  gboolean skip_alternate_angles;
  int max_titles;
} BroTitleSelection;

typedef struct {
  BroRipMode mode;
  BroBackupFormat backup_format;
  gboolean keep_backup_after_mkv;
  BroTitleSelection titles;
  int min_length_seconds; /* -1 = not set */
  int cache_mb;           /* -1 = not set */
  int direct_io;          /* -1 = not set, 0 = false, 1 = true */
  char *extra_arguments;
  gboolean write_disc_info_json;
  int format_modes[BRO_FORMAT_KEY_COUNT]; /* BroRipMode for DVDs, Blu-rays and 4K UHD discs; -1 = the drive's mode */
} BroRipConfig;

/* {name} - {episode} - {discLabel} - {rip} - {track} - {format}, leaving out parts that don't apply. */
#define BRO_DEFAULT_FILE_TEMPLATE "{name}{episode? - {episode}}{episodeTitle? - {episodeTitle}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}"
#define BRO_DEFAULT_FOLDER_TEMPLATE "{name}{discLabel? - {discLabel}}"
#define BRO_LEGACY_FOLDER_TEMPLATE "{disc}"
#define BRO_CONFIG_VERSION 2

typedef struct {
  char *root_override;
  char *folder_template;
  char *file_name_template;
  char *backup_subfolder;
  BroConflictPolicy conflict_policy;
  BroLibraryLayout layout;
} BroOutputConfig;

typedef struct {
  gboolean auto_rip_on_insert;
  int auto_rip_delay_seconds;
  gboolean eject_when_done;
  gboolean eject_on_failure;
  gboolean notify;
  gboolean play_sound;
  int wait_for_mount_seconds; /* before an automatic rip, wait up to this long for the disc to be mounted; 0 = don't */
  BroAlreadyArchived already_archived;
} BroAutomation;

/* What to do with discs that aren't DVDs or Blu-rays, when they are ripped automatically or with "Rip". */
typedef struct {
  gboolean rip_audio_cds;    /* with audio_command (cyanrip or abcde look the album up in MusicBrainz) */
  gboolean image_data_discs; /* data discs as ISO images */
  char *audio_command;       /* run in the output folder; {device} is the drive; "" = cyanrip, else abcde */
} BroOtherDiscs;

typedef struct {
  gboolean checksums;      /* SHA256SUMS in the output folder */
  gboolean archive_record; /* bromelia.json and the job log in the output folder */
  gboolean verify_rips;    /* check every ripped MKV against the disc listing with mkvmerge */
} BroArchiveConfig;

typedef struct {
  gboolean split_play_all;    /* split DVD "play all" titles of TV shows into episodes */
  gboolean keep_play_all;     /* keep the unsplit title too */
  gboolean read_menu_numbers; /* OCR episode numbers from the menus (ffmpeg + tesseract) */
} BroEpisodeConfig;

/* What a post-processing step runs: a program or script, or HandBrakeCLI with a preset. */
typedef enum { BRO_STEP_COMMAND, BRO_STEP_HANDBRAKE } BroStepKind;

#define BRO_HANDBRAKE_DEFAULT_PRESET "H.265 MKV 1080p30"
#define BRO_HANDBRAKE_DEFAULT_OUTPUT "{outputDir}/Encoded/{stem}.mkv"

typedef struct {
  char *id;
  char *name;
  gboolean enabled;
  char *executable;
  char *interpreter;
  char *arguments;
  char *working_directory;
  BroRunCondition run_on;
  gboolean per_file;
  int timeout_seconds;
  GHashTable *environment; /* char* -> char* */
  gboolean fail_job_on_error;
  char *match_name;        /* regular expression on the name / disc label; "" = all */
  GPtrArray *match_formats; /* char*: DVD, DVDe, BR, BRe, 4K, 4Ke, "BR*"…; empty = all */
  gboolean background;      /* run after the job, once the disc is out, in the background queue; can't fail the job */
  BroStepKind kind;
  char *preset;             /* HandBrake: a preset name (HandBrakeCLI --preset-list) */
  char *preset_file;        /* HandBrake: a preset file exported from HandBrake, or "" */
  char *output_path;        /* HandBrake: where each encode goes (template; the extension picks the container) */
  char *extra_arguments;    /* HandBrake: more HandBrakeCLI arguments */
} BroPostStep;

typedef struct {
  char *name;
  char *selection_rule;
  gboolean set_first_audio_default;
  gboolean set_first_subtitle_default;
  gboolean set_first_forced_subtitle_default;
  gboolean ignore_forced_subtitles_flag;
  gboolean use_iso639_2t;
  gboolean insert_first_chapter00;
  BroLpcmOutput lpcm_stereo;
  BroLpcmOutput lpcm_multichannel;
} BroGeneratedProfile;

typedef struct {
  BroProfileMode mode;
  char *custom_path;
  BroGeneratedProfile generated;
} BroProfileConfig;

typedef struct {
  char *id;
  char *name;
  gboolean enabled;
  char *match_drive_name;
  char *match_device;
  GHashTable *settings; /* MakeMKV setting overrides */
  BroProfileConfig profile;
  BroRipConfig rip;
  BroOutputConfig output;
  BroAutomation automation;
  BroArchiveConfig archive;
  BroEpisodeConfig episodes;
  BroOtherDiscs other;
  GPtrArray *post_process; /* BroPostStep* */
} BroDriveConfig;

typedef struct {
  char *id;
  char *name;
  BroDriveConfig *config;
} BroPreset;

typedef struct {
  BroMetadataProvider provider;
  char *api_key;  /* TMDb: API key (v3) or read access token; OMDb: API key */
  char *language; /* TMDb language, e.g. en-US */
  gboolean episode_titles; /* TV shows: look up the titles of the disc's episodes ({episodeTitle}) */
  gboolean nfo;            /* media server layout: write Kodi / Jellyfin / Emby .nfo files and the poster */
} BroMetadataConfig;

/* Where job notifications go: an http(s) webhook (Discord and Slack are recognised), ntfy://topic,
 * ntfys://host/topic, or any other Apprise URL (sent with the apprise command). */
typedef struct {
  char *id;
  char *url;
  gboolean enabled;
  gboolean only_problems; /* only jobs that didn't succeed */
} BroNotificationTarget;

typedef struct {
  gboolean enabled;
  char *address; /* 127.0.0.1 = this computer only; 0.0.0.0 = the network (needs a token) */
  int port;
  char *token;   /* required for everything when set (Authorization: Bearer <token> or ?token=) */
} BroWebUIConfig;

typedef struct {
  int version;
  char *makemkvcon_path;
  char *mkvmerge_path;
  char *output_root;
  int poll_interval_seconds;
  gboolean poll_while_ripping;
  int max_concurrent_jobs;
  char *registration_key;
  GHashTable *global_settings;
  BroDriveConfig *default_drive;
  GPtrArray *drives;  /* BroDriveConfig* */
  GPtrArray *presets; /* BroPreset* */
  GPtrArray *plugins; /* BroPostStep*: steps for every drive, usually limited by match_name / match_formats */
  int history_limit;
  int stall_timeout_minutes; /* stop a rip or backup that prints nothing for this long; 0 = never */
  gboolean prevent_sleep;    /* keep the computer awake while jobs run */
  BroMetadataConfig metadata;
  GPtrArray *notifications;  /* BroNotificationTarget* */
  gboolean auto_update_beta_key; /* register MakeMKV's current beta key at startup and when it has expired */
  int background_jobs;       /* background post-processing steps that run at the same time */
  BroWebUIConfig web_ui;
  int archive_check_interval_days; /* verify the output root's archives again every N days; 0 = never */
} BroAppConfig;

/* Enum <-> JSON string helpers (also used by labels in the UI). */
const char *bro_rip_mode_to_string (BroRipMode m);
const char *bro_rip_mode_label (BroRipMode m);
const char *bro_rip_mode_short (BroRipMode m);
gboolean    bro_rip_mode_makes_mkv (BroRipMode m);
gboolean    bro_rip_mode_makes_backup (BroRipMode m);
const char *bro_strategy_label (BroStrategy s);
const char *bro_run_condition_label (BroRunCondition r);
const char *bro_lpcm_to_string (BroLpcmOutput o);
const char *bro_lpcm_label (BroLpcmOutput o);
const char *bro_profile_mode_label (BroProfileMode m);
const char *bro_conflict_label (BroConflictPolicy p);
const char *bro_layout_label (BroLibraryLayout l);
const char *bro_already_archived_label (BroAlreadyArchived a);
const char *bro_metadata_provider_label (BroMetadataProvider p);
const char *bro_metadata_provider_to_string (BroMetadataProvider p);
/* The mode for a disc with format key (BroFormatKey; -1 = unknown) in automatic and quick rips. */
BroRipMode  bro_rip_config_mode_for (const BroRipConfig *r, int format_key);
const char *bro_format_key_name (BroFormatKey k); /* dvd, bluray, uhd */

BroNotificationTarget *bro_notification_target_new (void);
void                   bro_notification_target_free (BroNotificationTarget *t);

BroPostStep    *bro_post_step_new (void);
BroPostStep    *bro_post_step_new_handbrake (void); /* encodes every MKV in the background queue */
void            bro_post_step_free (BroPostStep *s);
BroPostStep    *bro_post_step_copy (const BroPostStep *s);

BroDriveConfig *bro_drive_config_new (void);
/* Settings for archiving everything on a disc: a decrypted backup kept next to MKV files of every title with every
 * track, the full disc listing, and all checks on. */
void            bro_drive_config_apply_archive_everything (BroDriveConfig *c);
void            bro_drive_config_free (BroDriveConfig *c);
BroDriveConfig *bro_drive_config_copy (const BroDriveConfig *c);
BroDriveConfig *bro_drive_config_apply_body (const BroDriveConfig *identity, const BroDriveConfig *body);
gboolean        bro_drive_config_matches (const BroDriveConfig *c, const BroDriveEntry *e);
JsonNode       *bro_drive_config_to_json (const BroDriveConfig *c);
BroDriveConfig *bro_drive_config_from_json (JsonNode *node);
void            bro_drive_config_upgrade_naming (BroDriveConfig *c);

BroPreset      *bro_preset_new (const char *name, const BroDriveConfig *config);
void            bro_preset_free (BroPreset *p);

BroAppConfig   *bro_app_config_new (void);
void            bro_app_config_free (BroAppConfig *c);
BroAppConfig   *bro_app_config_copy (const BroAppConfig *c);
JsonNode       *bro_app_config_to_json (const BroAppConfig *c);
BroAppConfig   *bro_app_config_from_json (JsonNode *node);
char           *bro_app_config_serialize (const BroAppConfig *c);
BroAppConfig   *bro_app_config_parse (const char *json, GError **error);
BroAppConfig   *bro_app_config_load (const char *path);
gboolean        bro_app_config_save (const BroAppConfig *c, const char *path, GError **error);

BroDriveConfig *bro_app_config_drive_for_entry (BroAppConfig *c, const BroDriveEntry *e);
BroDriveConfig *bro_app_config_drive_by_id (BroAppConfig *c, const char *id);
GHashTable     *bro_app_config_effective_settings (const BroAppConfig *c, const BroDriveConfig *d);
const char     *bro_app_config_output_root (const BroAppConfig *c, const BroDriveConfig *d);

char           *bro_normalize_drive_name (const char *s);
char           *bro_json_to_string (JsonNode *node, gboolean pretty);
gboolean        bro_write_file_atomic (const char *path, const char *contents, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroPostStep, bro_post_step_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveConfig, bro_drive_config_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroAppConfig, bro_app_config_free)

G_END_DECLS
