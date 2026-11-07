/* bro-keep-apart-step.h: BroKeepApartStep: The step JobService.keepApart puts in place of the step an interrupted,
 * failed or cancelled job stopped at and what follows (plan §20.1, §20.4): the job isn't continued, and what it left in
 * staging becomes a sealed unit in <library>/_Quarantine/<name> [INCOMPLETE] with a quarantined row of status
 * incomplete. JobOutcome makes the job cancelled once the step succeeded.
 *
 * Sealed as Seal seals, but with no produced lists, since Acquire never finished: every visible file in staging, plus
 * INCOMPLETE.txt for the outcome and error of the "outcome" checkpoint JobService stores with the step. Staging with no
 * such file (a job that stopped before Acquire has an empty one) holds nothing to keep: its empty folders are removed
 * and no unit is made.
 *
 * The checkpoint "sealed" is saved before the unit is handed to the library: a rerun with it doesn't seal again,
 * because staging may already be moving, and hands over the same unit. LibraryService picks the free name, finishes an
 * open intent and gives a unit quarantined already its result again, so a rerun rolls forward.
 *
 * <name>: Plan's identity name, else the request's title, the listing's name, the source's label or the job id, the
 * first that stays non-empty as a path component. Holds the library write. Data: CommitOutput (quarantined). Skipped
 * without Plan's output. */
#pragma once

#include "bro-bro-error.h"
#include "bro-outcome.h"
#include "bro-job-snapshot.h"
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

/* The job's (BroJobSnapshot) name in _Quarantine, before " [INCOMPLETE]" and ConflictNamer: the Plan output's identity
 * name, else the job's title, else Probe's listing name, else the source's label, else the job's id
 * (bro_sanitizer_component). RecoveryService sets a failed job's staging aside under it too. Transfer full. */
char *bro_keep_apart_step_folder_name (const BroJobSnapshot *job);

G_END_DECLS
