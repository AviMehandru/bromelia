/* bro-run-outcome.h: what one makemkvcon run amounted to. makemkvcon exits 0 when a title fails, so the saved /
 * failed counts (5036, 5037, 5004) and the files the run produced decide, not the exit status alone. */
#pragma once

#include "bro-process-exit.h"
#include "bro-run-accumulator.h"
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
} BroRunOutcome;

/* Cancelled when the run was cancelled. Failed when MakeMKV's space warning or a renumbered drive stopped it, when
 * it stalled, exited non-zero, reported a failed title or produced nothing; the error is the reason (the first
 * error MakeMKV reported, else the last, else the exit status). Errors when it read the disc with read errors
 * (rip.readErrors). Success otherwise. */
BroRunOutcome *bro_run_outcome_classify (const BroRunAccumulator *accumulator, const BroProcessExit *exit, int produced_files);
void bro_run_outcome_free (BroRunOutcome *outcome);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunOutcome, bro_run_outcome_free)

G_END_DECLS
