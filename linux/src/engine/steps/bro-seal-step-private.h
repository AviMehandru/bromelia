/* bro-seal-step-private.h: what SealStep shares with KeepApartStep, which seals an interrupted, failed or cancelled
 * job's staging the same way, and with RecoveryService, which copies a set-aside job's logs the same way. */
#pragma once

#include "bro-step-context.h"

G_BEGIN_DECLS

/* What one run of a sealing step has. */
typedef struct {
  const BroStepContext *c;
  const char *staging;
  GPtrArray *written; /* char *: Bromelia's own files this run wrote */
} BroSealRun;

/* job.cancelled once the job is cancelled. */
gboolean _bro_seal_cancelled (const BroStepContext *c, BroBroError **error);

/* A file's SHA-256 (g_free it) and size, read through the FileSystem port (from disk when @from_disk);
 * archive.hashFailed when it can't be read; job.cancelled between two pieces. NULL and @error set on failure. */
char *_bro_seal_hash (const BroStepContext *c, const char *path, const char *file, gboolean from_disk, gint64 expected, gint64 *size, BroBroError **error);

/* Every folder from @staging down to each of @files (char *, relative), once each, in byte order (yours to unref). */
GPtrArray *_bro_seal_folders (const char *staging, GPtrArray *files);

/* Writes @name into staging atomically (0644) and notes it written; archive.writeFailed with the cause. */
gboolean _bro_seal_write (BroSealRun *r, const char *name, GBytes *bytes, BroBroError **error);
gboolean _bro_seal_write_text (BroSealRun *r, const char *name, const char *text, BroBroError **error);
gboolean _bro_seal_was_written (BroSealRun *r, const char *name);

/* The job's log (BroLogLineDto *, yours to unref) and, when @copy, the copies written into staging: the log as text
 * (bromelia-<unit8>-log.txt) and MakeMKV's transcripts (makemkv-<unit8>-log.txt, makemkv-<unit8>-debug-log.txt), each when
 * the job folder has it; their names appended to @logs (char *). NULL and @error set on failure. */
GPtrArray *_bro_seal_copy_logs (BroSealRun *r, const char *unit8, gboolean copy, GPtrArray *logs, BroBroError **error);

/* Writes one log copy called @name; FALSE and @error set when it can't. */
typedef gboolean (*BroSealWriteFunc) (gpointer data, const char *name, GBytes *bytes, BroBroError **error);

/* _bro_seal_copy_logs from the job folder @job_directory, each copy handed to @write: RecoveryService's set-aside
 * copies a failed or cancelled job's logs the same way. */
GPtrArray *_bro_seal_copy_logs_from (BroFileSystem *fs, const char *job_directory, BroMessageRenderer *messages, const char *unit8, gboolean copy,
                                     BroSealWriteFunc write, gpointer data, GPtrArray *logs, BroBroError **error);

G_END_DECLS
