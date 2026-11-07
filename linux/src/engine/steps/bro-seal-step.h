/* bro-seal-step.h: BroSealStep: Step 13 (plan §20.1; §22.3 steps 1–3): makes the job's staging folder a sealed unit, so
 * the commit only moves it. It seals the produced items by name, never by a count: Name's output, else Acquire's names
 * as they are. An item that isn't in staging or holds no file (an empty folder, a hidden path, a path through '..')
 * fails before anything is touched, and so does a visible file no step made; leftovers of a stopped write are removed.
 *
 * In order: every file is synced, then every folder from staging down to each file, so each entry on the way to a file
 * is durable; every file is hashed read back from disk (archive.hashFromDisk, on by default); then SHA256SUMS, the
 * logs, the READ ERRORS note and the record are written, each atomically. Read back from disk goes past the system
 * cache only on Windows: macOS and Linux ask the cache to stay out of the way, and a page still in it can answer
 * (FileSystem.openForReading).
 *
 * SHA256SUMS and the record are always written: archive.checksums: false is ignored and archive.archiveRecord: false
 * leaves out only the logs. Nothing is trusted from an earlier attempt: a rerun syncs and hashes everything again and
 * writes the same files. Read errors tag the unit "READ ERRORS", so Commit puts it in _Quarantine/<name> [READ ERRORS]
 * (plan §22.2). Data: SealOutput. Skipped when Plan or Acquire hasn't run. */
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
