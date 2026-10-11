/* bro-step-report.h: what a step's run says: its output data (for the steps after it), the steps it appends (Plan),
 * whether it skipped, what the job's outcome needs (read errors, quarantine, the MakeMKV notice that explains a
 * failure, a reason to skip the whole job, whether its failure counts), and its error when it failed. */
#pragma once

#include "bro-bro-error.h"
#include "bro-bro-message.h"
#include "bro-makemkv-notice.h"
#include "bro-step-kind.h"

G_BEGIN_DECLS

typedef struct {
  BroJsonValue *data;       /* never NULL */
  BroBroMessage *summary;   /* nullable */
  GArray *append_steps;     /* BroStepKind */
  gboolean skipped;
  int read_errors;
  gboolean quarantined;
  BroMakemkvNotice *notice; /* nullable */
  BroBroMessage *skip_job;  /* nullable */
  BroBroError *error;       /* nullable: failed when set */
  gboolean affects_outcome;
} BroStepReport;

/* Takes ownership of @data (NULL: {}); counts, affects the outcome. */
BroStepReport *bro_step_report_new (BroJsonValue *data);
void bro_step_report_free (BroStepReport *report);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepReport, bro_step_report_free)

G_END_DECLS
