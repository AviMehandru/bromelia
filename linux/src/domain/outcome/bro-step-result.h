/* bro-step-result.h: what a finished step contributes to its job's outcome: its state and error, the read errors
 * it saw, whether it put the unit in quarantine, the MakeMKV notice that explains a failure, the reason to skip
 * the job (a disc archived before), and whether its failure counts (post-processing steps). */
#pragma once

#include "bro-bro-error.h"
#include "bro-bro-message.h"
#include "bro-makemkv-notice.h"
#include "bro-step-kind.h"
#include "bro-step-state.h"

G_BEGIN_DECLS

typedef struct {
  BroStepKind kind;
  BroStepState state;
  BroBroError *error; /* nullable */
  int read_errors;
  gboolean quarantined;
  BroMakemkvNotice *notice; /* nullable */
  BroBroMessage *skip;      /* nullable */
  gboolean affects_outcome;
} BroStepResult;

/* No error, read errors, notice or skip; affects the outcome. */
BroStepResult *bro_step_result_new (BroStepKind kind, BroStepState state);
void bro_step_result_free (BroStepResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepResult, bro_step_result_free)

G_END_DECLS
