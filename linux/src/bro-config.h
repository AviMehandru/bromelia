/* bro-config.h — configuration model (same JSON schema as the macOS and Windows versions). */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>
#include "bro-robot.h"

G_BEGIN_DECLS

typedef enum { BRO_STRATEGY_ALL, BRO_STRATEGY_LONGEST, BRO_STRATEGY_INDICES, BRO_STRATEGY_MANUAL } BroStrategy;
typedef enum { BRO_INDEX_MAKEMKV, BRO_INDEX_SOURCE } BroIndexBase;
typedef enum { BRO_MODE_MKV, BRO_MODE_BACKUP, BRO_MODE_BACKUP_DECRYPTED, BRO_MODE_BACKUP_THEN_MKV, BRO_MODE_INFO_ONLY } BroRipMode;
typedef enum { BRO_BACKUP_FOLDER, BRO_BACKUP_ISO } BroBackupFormat;
typedef enum { BRO_CONFLICT_UNIQUE, BRO_CONFLICT_OVERWRITE, BRO_CONFLICT_SKIP } BroConflictPolicy;
typedef enum { BRO_RUN_SUCCESS, BRO_RUN_FAILURE, BRO_RUN_ALWAYS } BroRunCondition;
typedef enum { BRO_PROFILE_MAKEMKV_DEFAULT, BRO_PROFILE_GENERATED, BRO_PROFILE_CUSTOM_FILE } BroProfileMode;
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
} BroRipConfig;

/* {name} - {episode} - {discLabel} - {rip} - {track} - {format}, leaving out parts that don't apply. */
#define BRO_DEFAULT_FILE_TEMPLATE "{name}{episode? - {episode}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}"
#define BRO_DEFAULT_FOLDER_TEMPLATE "{name}{discLabel? - {discLabel}}"
#define BRO_LEGACY_FOLDER_TEMPLATE "{disc}"
#define BRO_CONFIG_VERSION 2

typedef struct {
  char *root_override;
  char *folder_template;
  char *file_name_template;
  char *backup_subfolder;
  BroConflictPolicy conflict_policy;
} BroOutputConfig;

typedef struct {
  gboolean auto_rip_on_insert;
  int auto_rip_delay_seconds;
  gboolean eject_when_done;
  gboolean eject_on_failure;
  gboolean notify;
  gboolean play_sound;
} BroAutomation;

typedef struct {
  gboolean checksums;      /* SHA256SUMS in the output folder */
  gboolean archive_record; /* bromelia.json and the job log in the output folder */
} BroArchiveConfig;

typedef struct {
  gboolean split_play_all;    /* split DVD "play all" titles of TV shows into episodes */
  gboolean keep_play_all;     /* keep the unsplit title too */
  gboolean read_menu_numbers; /* OCR episode numbers from the menus (ffmpeg + tesseract) */
} BroEpisodeConfig;

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
  GPtrArray *post_process; /* BroPostStep* */
} BroDriveConfig;

typedef struct {
  char *id;
  char *name;
  BroDriveConfig *config;
} BroPreset;

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

BroPostStep    *bro_post_step_new (void);
void            bro_post_step_free (BroPostStep *s);
BroPostStep    *bro_post_step_copy (const BroPostStep *s);

BroDriveConfig *bro_drive_config_new (void);
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

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveConfig, bro_drive_config_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroAppConfig, bro_app_config_free)

G_END_DECLS
