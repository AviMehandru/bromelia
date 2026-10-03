/* bro-step-filter.h: BroStepFilter, which post-processing steps run after a job, and the status word scripts and
 * notifications get. */
#pragma once

#include "bro-outcome.h"
#include "bro-status-word.h"
#include "bro-step-definition.h"

G_BEGIN_DECLS

/* An enabled step runs on success (succeeded, or succeeded with a failed post-processing step), on failure (failed,
 * cancelled, interrupted, or succeeded with read errors), or always; skipped jobs never run steps. */
gboolean bro_step_filter_applies (const BroStepDefinition *step, BroOutcome outcome);

/* success (succeeded, or a failed post-processing step: the archive is fine), errors (read errors), failed (also
 * interrupted), cancelled, skipped. */
BroStatusWord bro_step_filter_status_word (BroOutcome outcome);

G_END_DECLS
