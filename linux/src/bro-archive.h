/* bro-archive.h — checking archives already made: disc fingerprints (is this disc archived already?) and verifying
 * SHA256SUMS folders again (bit rot, bad copies). Same rules as the macOS and Windows versions. */
#pragma once

#include <gio/gio.h>
#include "bro-robot.h"

G_BEGIN_DECLS

/* ---- disc fingerprints ---- */

/* "v1:" + 32 hex digits of SHA-256 over the volume name, the title count and, sorted, each title's source title id,
 * length in seconds, segment map and size in bytes. NULL for a listing without titles. */
char *bro_disc_fingerprint (BroDiscInfo *info);

/* Where a disc was archived before. */
typedef struct {
  char *folder;
  gint64 archived_at; /* unix seconds, 0 when unknown */
} BroArchivedMatch;

void bro_archived_match_free (BroArchivedMatch *m);

/* A finished job that may have archived a disc (from the history). */
typedef struct {
  char *fingerprint;
  char *folder;
  char *state;        /* history state word: success, errors, failed, cancelled */
  gint64 finished_at;
} BroArchivedCandidate;

void bro_archived_candidate_free (BroArchivedCandidate *c);

/* The first candidate (newest first) that archived fingerprint successfully and whose folder still exists; else, when
 * root is set, the first bromelia*.json with status "success" and that fingerprint in root or up to four folders
 * below it. NULL when the disc isn't archived. */
BroArchivedMatch *bro_find_archived (const char *fingerprint, GPtrArray *candidates /* BroArchivedCandidate*, may be NULL */,
                                     const char *root);

/* ---- verifying archives ---- */

typedef struct {
  char *folder;
  int files;             /* entries in SHA256SUMS */
  gint64 bytes;          /* bytes hashed */
  GPtrArray *missing;    /* char*: listed, not there */
  GPtrArray *changed;    /* char*: different contents */
  GPtrArray *unreadable; /* char*: couldn't be read to the end */
  GPtrArray *extra;      /* char*: files in the folder that SHA256SUMS doesn't list (not an error) */
  char *error;           /* SHA256SUMS couldn't be read, or NULL */
} BroFolderCheck;

void     bro_folder_check_free (BroFolderCheck *r);
gboolean bro_folder_check_ok (const BroFolderCheck *r); /* nothing missing, changed or unreadable */
char    *bro_folder_check_summary (const BroFolderCheck *r); /* "12 files OK" / "1 changed, 2 missing" … */

/* Folders holding a SHA256SUMS: path itself and every folder below it (hidden folders skipped), sorted. */
GPtrArray *bro_archive_folders (const char *path);

/* Progress while verifying: bytes hashed so far of the total, and the file being read. Return FALSE to stop. */
typedef gboolean (*BroVerifyProgress) (gint64 done, gint64 total, const char *folder, const char *file, gpointer user_data);

/* Re-hashes every file of every folder (blocking; call on a worker thread). Returns BroFolderCheck* for the folders
 * finished; *stopped is set when progress asked to stop. */
GPtrArray *bro_verify_folders (GPtrArray *folders, BroVerifyProgress progress, gpointer user_data, gboolean *stopped);

/* ---- when folders were last checked (archive-checks.json in the data folder) ---- */

typedef struct {
  gint64 checked_at; /* unix seconds */
  gboolean ok;
  char *summary;
} BroCheckRecord;

void            bro_check_record_free (BroCheckRecord *r);
/* folder -> BroCheckRecord*; empty when the file doesn't exist. */
GHashTable     *bro_check_records_load (const char *path);
gboolean        bro_check_records_save (GHashTable *records, const char *path);
void            bro_check_records_add (GHashTable *records, const BroFolderCheck *r, gint64 when);

/* ---- episode numbering across discs ---- */

/* A TV disc's place in its set: the show's name (after the online lookup), the title read from the disc label, and
 * season / part / volume (-1 when absent) and disc number. */
typedef struct {
  const char *name;
  const char *label_title;
  int season, part, volume, disc;
} BroContinuationQuery;

typedef struct {
  int last_episode;
  char *source; /* where it was found, for the log */
} BroContinuation;

void             bro_continuation_free (BroContinuation *c);
/* Where the episode numbering of a disc continues: after the last episode of the previous disc of its set (same show,
 * season, part and volume; disc number one lower), read from the archive records (bromelia*.json, status success or
 * errors, kind tv) in folders (char*, the history's; may be NULL) and in root and up to four folders below it; else, when
 * season_folder is set (a media server library) and no record shows a later disc, the highest SxxEyy of season in its
 * file names. NULL for disc 1 or when nothing is found. */
BroContinuation *bro_episode_continuation (const BroContinuationQuery *q, const char *root, GPtrArray *folders,
                                           const char *season_folder, int season);
/* The highest episode number of season in the names of the files in folder (… S02E05 …), or -1. */
int              bro_highest_episode (const char *folder, int season);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroArchivedMatch, bro_archived_match_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroContinuation, bro_continuation_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroFolderCheck, bro_folder_check_free)

G_END_DECLS
