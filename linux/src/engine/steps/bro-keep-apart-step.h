/* bro-keep-apart-step.h: BroKeepApartStep: the step JobService.keepApart puts in place of the step an interrupted, failed
 * or cancelled job stopped at and what follows (owner, 2026-10-03 and 2026-10-25; plan §20.1, §20.4): the job isn't
 * continued, and what it left in staging becomes a sealed unit in <library>/_Quarantine/<name> [INCOMPLETE] with a
 * quarantined row of status incomplete.
 * 1. Sealed as Seal seals, but with no produced lists (Acquire never finished): every visible file in staging (none
 *    hidden by its first byte, not one of Bromelia's own at the top; leftovers of a stopped write removed) is synced,
 *    then each folder down to it (archive.synced), and hashed read back from disk (archive.hashFromDisk) with
 *    archive.hashing progress. SHA256SUMS, the logs as Seal copies them, INCOMPLETE.txt (the note recovery writes, with
 *    the logs, for the outcome and error of the "outcome" checkpoint JobService stores with the step: interrupted
 *    without one) and the record (status incomplete, no titles acquired) are written atomically; an earlier attempt's
 *    own files that this one doesn't write go. Staging with no such file (a job that stopped before Acquire has an empty
 *    one) holds nothing to keep: its empty folders are removed and no unit is made.
 * 2. The checkpoint "sealed" (the files and Bromelia's own files) is saved before the unit is handed to the library: a
 *    rerun with it doesn't seal again (staging may already be moving) and hands over the same unit.
 * 3. library.quarantine(unit, intent, files, "INCOMPLETE"): the intent's items are staging's top-level entries that hold
 *    the files (a file with its SHA-256, a folder without), then the record, the logs and the note; LibraryService picks
 *    the free name, finishes an open intent and gives a unit quarantined already its result again, so a rerun rolls
 *    forward. recovery.keptApart {path} (warning).
 * <name>: Plan's identity name, else the request's title, else the listing's name, else the source's label, else the job
 * id, the first that stays non-empty as a path component. Holds the library write; idempotent. Data: CommitOutput
 * (quarantined). Skipped without Plan's output. JobOutcome makes the job cancelled once the step succeeded. */
#pragma once

#include "bro-bro-error.h"
#include "bro-outcome.h"
#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_KEEP_APART_STEP_TAG "INCOMPLETE"
#define BRO_KEEP_APART_STEP_NOTE "INCOMPLETE.txt"
#define BRO_KEEP_APART_STEP_SEALED_CHECKPOINT "sealed"
#define BRO_KEEP_APART_STEP_OUTCOME_CHECKPOINT "outcome"

#define BRO_TYPE_KEEP_APART_STEP (bro_keep_apart_step_get_type ())
G_DECLARE_FINAL_TYPE (BroKeepApartStep, bro_keep_apart_step, BRO, KEEP_APART_STEP, GObject)

BroKeepApartStep *bro_keep_apart_step_new (void);

/* The checkpoint JobService.keepApart stores with the step: {kind: "outcome", outcome, error?}, the outcome the job had
 * (interrupted, failed, cancelled) and its error, which the note names. Transfer full. */
BroJsonValue *bro_keep_apart_step_kept (BroOutcome outcome, const BroBroError *error);

G_END_DECLS
