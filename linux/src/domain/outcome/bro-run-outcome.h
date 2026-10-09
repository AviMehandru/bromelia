/* bro-run-outcome.h: what one makemkvcon run amounted to. makemkvcon exits 0 when a title fails, so the saved /
 * failed counts (5036, 5037, 5004) and the files the run produced decide, not the exit status alone. */
#pragma once

#include "bro-process-exit.h"
#include "bro-run-accumulator.h"
#include "bro-run-product.h"
#include "bro-status-word.h"

G_BEGIN_DECLS

typedef struct {
  gboolean has_saved;
  int saved;
  gboolean has_failed;
  int failed;
  int exit_code;
  BroRobotMessage *first_error;   /* nullable */
  BroRobotMessage *space_warning; /* nullable */
  GPtrArray *read_errors;         /* BroRobotMessage * */
  BroBroMessage *drive_mismatch;  /* nullable */
  char *debug_log;                /* nullable */
  BroStatusWord status;
  BroBroMessage *error;           /* nullable */
  GPtrArray *produced;            /* char *: the new names that count (bro_run_outcome_products) */
} BroRunOutcome;

/* Cancelled when the run was cancelled. Failed when MakeMKV's space warning or a renumbered drive stopped it, when
 * it stalled, exited non-zero, reported a failed title or produced nothing; the error is the reason (the first
 * error MakeMKV reported, else the last, else the exit status, else process.savedNothing). A rip
 * (BRO_RUN_PRODUCT_TITLES) also fails when MakeMKV didn't say how many titles it saved (makemkv.noSummary) or when
 * that number isn't the number of new MKV files (makemkv.savedMismatch). Errors when it read the disc with read errors
 * (rip.readErrors). Success otherwise. @new_names (char *) are the names in the destination that weren't there before
 * the run; only bro_run_outcome_products of them count, and they become ->produced. */
BroRunOutcome *bro_run_outcome_classify (const BroRunAccumulator *accumulator, const BroProcessExit *exit, BroRunProduct product,
                                         GPtrArray *new_names);

/* The new names (char *) a run of this kind produced, in the order given: MKV files for a rip (a name ending in .mkv,
 * in any case, that isn't hidden), the disc structure for a backup (a BDMV, VIDEO_TS or HVDVD_TS folder, or an .iso
 * image), the file itself for an image (any name that isn't hidden), nothing for a listing. Anything else (.DS_Store,
 * Thumbs.db, a partial file) doesn't count. Free with g_ptr_array_unref. */
GPtrArray *bro_run_outcome_products (BroRunProduct product, GPtrArray *new_names);
void bro_run_outcome_free (BroRunOutcome *outcome);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunOutcome, bro_run_outcome_free)

G_END_DECLS
