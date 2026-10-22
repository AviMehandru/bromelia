/* bro-seal-step.h: BroSealStep: Step 13 (plan §20.1; §22.3 steps 1–3): makes the job's staging folder a sealed unit, so
 * the commit only moves it. What it seals is the produced items by name (review L3-1: the lists, never a count): Name's
 * output, else Acquire's names as they are (bro_name_output_from_acquire). An item that isn't in staging, or holds no
 * file (an empty folder, a hidden path, a path through '..'; owner, 2026-10-21), fails with archive.fileMissing before
 * anything is touched, a visible file no step made with archive.unlisted (owner, 2026-10-20), and leftovers of a stopped
 * write are removed. Then, in order:
 * 1. every file is synced to disk, then every folder from staging down to each file (archive.synced): a folder that
 *    holds only folders (a backup's) is synced too, so each entry on the way to a file is durable;
 * 2. every file is hashed read back from disk (archive.hashFromDisk, on by default: the cache is bypassed), with
 *    archive.hashing progress; a file that can't be read fails with archive.hashFailed;
 * 3. SHA256SUMS, the logs (the job's log.jsonl rendered line by line, MakeMKV's transcripts as they are), the READ
 *    ERRORS note when MakeMKV reported read errors, and the record are written, each atomically; a write that fails
 *    fails with archive.writeFailed. Bromelia's own files from an earlier attempt that this one doesn't write go.
 * SHA256SUMS and the record are always written; archive.checksums: false is ignored (archive.checksumsRequired) and
 * archive.archiveRecord: false leaves out only the logs (owner, 2026-10-20). Nothing is trusted from an earlier attempt:
 * a rerun syncs and hashes everything again and writes the same files. Read errors give the unit the tag "READ ERRORS"
 * (Commit puts it in _Quarantine/<name> [READ ERRORS], plan §22.2) and the record's status errors. Data: BroSealOutput.
 * Skipped when Plan or Acquire hasn't run. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_SEAL_STEP_READ_ERRORS_TAG "READ ERRORS"
#define BRO_SEAL_STEP_READ_ERRORS_NOTE "READ ERRORS.txt"
/* The job folder's MakeMKV transcripts (MakemkvService's makemkv.txt; MakeMKV's debug log). */
#define BRO_SEAL_STEP_TRANSCRIPT "makemkv.txt"
#define BRO_SEAL_STEP_DEBUG_TRANSCRIPT "makemkv-debug.txt"

#define BRO_TYPE_SEAL_STEP (bro_seal_step_get_type ())
G_DECLARE_FINAL_TYPE (BroSealStep, bro_seal_step, BRO, SEAL_STEP, GObject)

BroSealStep *bro_seal_step_new (void);

/* The job's log as text (g_free it): per line (@lines: BroLogLineDto *) its time (UTC), [DEBUG] / [WARNING] / [ERROR]
 * unless it is info, and the message in the configured language. */
char *bro_seal_step_log_text (GPtrArray *lines, BroMessageRenderer *messages);

G_END_DECLS
