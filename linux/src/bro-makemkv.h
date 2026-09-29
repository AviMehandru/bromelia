/* bro-makemkv.h — settings.conf, profiles, per-drive environments, command lines and the settings catalog. */
#pragma once

#include <gio/gio.h>
#include "bro-config.h"

G_BEGIN_DECLS

#define BRO_DEFAULT_SELECTION "-sel:all,+sel:(favlang|nolang|single),-sel:(havemulti|havecore),-sel:mvcvideo,=100:all,-10:favlang"

/* ---- sources ---- */

typedef enum { BRO_SOURCE_DRIVE, BRO_SOURCE_ISO, BRO_SOURCE_FOLDER } BroSourceKind;

typedef struct {
  BroSourceKind kind;
  int index;       /* drive number (drives) */
  char *path;      /* device path (drives) or file / folder path */
} BroSource;

BroSource *bro_source_new_drive (int index, const char *device);
BroSource *bro_source_new_path (BroSourceKind kind, const char *path);
BroSource *bro_source_copy (const BroSource *s);
void       bro_source_free (BroSource *s);
char      *bro_source_info_argument (const BroSource *s);
char      *bro_source_backup_argument (const BroSource *s); /* NULL unless a drive */
char      *bro_source_display_name (const BroSource *s);

/* ---- files ---- */

GHashTable *bro_settings_conf_parse (const char *text);
char       *bro_settings_conf_serialize (GHashTable *settings, const char *header);
char       *bro_profile_build (const BroGeneratedProfile *p, const char *selection_override);

/* The user's own MakeMKV folder (~/.MakeMKV, or ~/Library/MakeMKV on macOS for development). */
char       *bro_makemkv_user_folder (void);
GHashTable *bro_makemkv_installed_settings (void);

/* ---- environment ---- */

typedef struct {
  char *executable;
  char *home;
  char *profile_path; /* may be NULL */
  GHashTable *settings;
} BroMakemkvEnv;

BroMakemkvEnv *bro_makemkv_env_prepare (const char *executable, const BroAppConfig *config, const BroDriveConfig *drive,
                                        const char *home, const char *selection_override, GError **error);
void           bro_makemkv_env_free (BroMakemkvEnv *env);
char         **bro_makemkv_env_environ (const BroMakemkvEnv *env);
GPtrArray     *bro_makemkv_common_args (const BroMakemkvEnv *env, const BroRipConfig *rip);
GPtrArray     *bro_makemkv_info_args (const BroMakemkvEnv *env, const BroSource *src, const BroRipConfig *rip);
GPtrArray     *bro_makemkv_mkv_args (const BroMakemkvEnv *env, const BroSource *src, const char *title, const char *dest, const BroRipConfig *rip);
GPtrArray     *bro_makemkv_backup_args (const BroMakemkvEnv *env, const BroSource *src, gboolean decrypt, const char *dest, const BroRipConfig *rip);
GPtrArray     *bro_makemkv_scan_args (void);

char *bro_find_tool (const char *configured, const char *name);

/* ---- catalog ---- */

typedef struct {
  char *key, *label, *type, *def, *help;
  GPtrArray *choice_values, *choice_labels; /* may be NULL */
  gboolean advanced;
  gboolean this_platform;
} BroCatalogSetting;

typedef struct {
  char *id, *title;
  GPtrArray *settings; /* BroCatalogSetting* */
} BroCatalogSection;

typedef struct {
  GPtrArray *sections;          /* BroCatalogSection* */
  GPtrArray *token_names, *token_helps;
  GPtrArray *preset_names, *preset_rules;
} BroCatalog;

BroCatalog *bro_catalog_load_from_data (const char *json, GError **error);
BroCatalog *bro_catalog_get (void); /* from GResource, cached */
gboolean    bro_catalog_has_key (BroCatalog *c, const char *key);
GPtrArray  *bro_catalog_keys (BroCatalog *c);

/* ---- processes ---- */

typedef void (*BroLineFunc) (const char *line, gpointer user_data);

/* Runs argv synchronously (call from a worker thread), feeding every output line to func.
 * `handle` (optional) receives the running process so another thread can cancel it. */
gboolean bro_process_run (const char *const *argv, const char *const *envp, const char *cwd, int timeout_seconds,
                          GCancellable *cancellable, BroLineFunc func, gpointer user_data,
                          int *exit_status, gboolean *was_cancelled, gboolean *timed_out, GError **error);

typedef struct {
  int timeout_seconds; /* 0 = none */
  int stall_seconds;   /* stop the process when it prints nothing for this long; 0 = never */
  int stop_signal;     /* sent first when stopping (SIGKILL follows after 5 s); 0 = SIGINT */
} BroRunOptions;

typedef struct {
  int exit_status;     /* -1 when it didn't exit normally */
  gboolean cancelled, timed_out;
  gboolean stalled;    /* stopped because it printed nothing for stall_seconds */
  gboolean abandoned;  /* didn't exit even after SIGKILL; no longer waited for */
} BroRunStatus;

/* Like bro_process_run, with a stall watchdog and a configurable stop signal. */
gboolean bro_process_run_ex (const char *const *argv, const char *const *envp, const char *cwd, const BroRunOptions *opt,
                             GCancellable *cancellable, BroLineFunc func, gpointer user_data, BroRunStatus *out, GError **error);

void bro_ptr_array_add_all (GPtrArray *a, const char *first, ...) G_GNUC_NULL_TERMINATED;
char **bro_ptr_array_to_strv (GPtrArray *a);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSource, bro_source_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvEnv, bro_makemkv_env_free)

/* MakeMKV's free beta key, which the developer posts on the forum and replaces about once a month. */
#define BRO_BETA_KEY_URL "https://forum.makemkv.com/forum/viewtopic.php?f=5&t=1053"
char      *bro_beta_key_parse (const char *html); /* the "T-…" key in the post, or NULL */
char      *bro_beta_key_fetch (GError **error);   /* downloads the forum page with curl; blocking */
/* Whether an automatic update may replace installed: only a beta key (T-…) or no key, never a purchased one. */
gboolean   bro_beta_key_may_replace (const char *installed);

/* Where a file or folder the user opened leads: MakeMKV opens discs (an image, or a folder holding BDMV / VIDEO_TS /
 * HVDVD_TS), not single files, so a file inside a disc structure (.IFO, .VOB, .mpls, .m2ts, …) opens its disc. */
BroSource *bro_source_resolve (const char *path, gboolean is_dir);

G_END_DECLS
