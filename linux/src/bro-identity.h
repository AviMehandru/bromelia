/* bro-identity.h — what a disc is (format, movie / show name, place in a set), plugin matching and
 * SHA-256 checksums for archiving. Same rules as the macOS and Windows versions. */
#pragma once

#include <glib.h>
#include "bro-config.h"
#include "bro-robot.h"

G_BEGIN_DECLS

typedef enum { BRO_FORMAT_UNKNOWN, BRO_FORMAT_DVD, BRO_FORMAT_BLURAY, BRO_FORMAT_UHD, BRO_FORMAT_HDDVD } BroDiscFormat;
typedef enum { BRO_KIND_MOVIE, BRO_KIND_TV } BroMediaKind;

/* Format code used in file names: DVD, BR, 4K, HDDVD, DISC; "e" suffix for backups that are not decrypted. */
char         *bro_format_code (BroDiscFormat f, gboolean encrypted);
const char   *bro_format_label (BroDiscFormat f);
const char   *bro_format_token (BroDiscFormat f); /* dvd, bluray, uhd, hddvd, unknown */
const char   *bro_kind_label (BroMediaKind k);    /* Movie, TV show */
int           bro_format_key (BroDiscFormat f);   /* BroFormatKey for rip.formatModes, -1 for other formats */
const char   *bro_kind_token (BroMediaKind k);    /* movie, tv */
extern const char *const bro_format_codes[];      /* NULL terminated: DVD, DVDe, BR, BRe, 4K, 4Ke */

/* From makemkvcon's listing (UHD = Blu-ray with 2160p or HEVC video), falling back to DRV flags (-1 = unknown). */
BroDiscFormat bro_detect_format (BroDiscInfo *info, int drive_flags);
/* Backup folder: VIDEO_TS = DVD, BDMV/index.bdmv "INDX0300" = UHD. Returns BRO_FORMAT_UNKNOWN when unsure. */
BroDiscFormat bro_detect_backup_folder (const char *folder);

typedef struct {
  char *title;
  int season, part, volume, disc; /* -1 when absent */
  gboolean looks_like_series;
} BroLabelInfo;

void  bro_label_parse (const char *label, BroLabelInfo *out); /* fills out; free with bro_label_clear */
void  bro_label_clear (BroLabelInfo *l);
char *bro_label_set_description (const BroLabelInfo *l); /* "Season 2 Part 7 Disc 2" or "" */

typedef struct {
  char *name;
  BroMediaKind kind;
  BroDiscFormat format;
  gboolean encrypted;
  BroLabelInfo label;
  char *reason;
} BroIdentity;

/* name_override: "" = infer; kind_override: -1 = infer; play_all_episodes: episodes found in a DVD play-all title. */
BroIdentity *bro_identity_resolve (BroDiscInfo *info, const char *disc_label, int drive_flags, int format_override,
                                   gboolean encrypted, const char *name_override, int kind_override, int play_all_episodes);
void         bro_identity_free (BroIdentity *id);
gboolean     bro_identity_equal (const BroIdentity *a, const BroIdentity *b);
char        *bro_identity_format_code (const BroIdentity *id);
/* Sets name, kind, format, rip, discLabel, discNumber, season, part, volumeNumber and empty episode / episodeNumber / track. */
void         bro_identity_template_values (const BroIdentity *id, GHashTable *values, const char *rip);

/* Titles of 10–75 minutes within ±35 % of the median of such titles (BroTitle*, not owned). */
GPtrArray *bro_episode_like_titles (GPtrArray *titles);
char      *bro_track_label (BroTitle *t);   /* Title 11 / Playlist 00800 */
char      *bro_episode_label (int n, int width);

gboolean bro_plugin_matches (const BroPostStep *step, const char *name, const char *disc_label, const char *format_code);
char    *bro_plugin_validate (const char *pattern); /* NULL when valid */

/* ---- checksums ---- */

#define BRO_CHECKSUM_FILE "SHA256SUMS"

typedef struct {
  char *path; /* relative to the output folder, '/' separated */
  gint64 size;
  char *sha256;
} BroChecksum;

void       bro_checksum_free (BroChecksum *c);
/* Regular files under items (folders walked recursively), as BroChecksum with path + size (no hash), sorted. */
GPtrArray *bro_checksum_list_files (GPtrArray *items, const char *base);
typedef gboolean (*BroHashProgress) (gint64 done, gpointer user_data); /* return FALSE to cancel */
char      *bro_sha256_file (const char *path, BroHashProgress progress, gpointer user_data, GError **error);
char      *bro_checksums_render (GPtrArray *entries); /* "hash  path\n"… */
/* Writes base/SHA256SUMS keeping entries of earlier jobs; returns the path. */
char      *bro_checksums_write_merged (const char *base, GPtrArray *entries, GError **error);
/* Re-hashes base/SHA256SUMS; returns the paths (char*) that are missing or differ, or NULL on error. */
GPtrArray *bro_checksums_verify (const char *base, GError **error);

G_END_DECLS
