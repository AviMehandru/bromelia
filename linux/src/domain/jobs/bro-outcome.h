/* bro-outcome.h: how a job ended (plan §20.2) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_OUTCOME_SUCCEEDED,
  BRO_OUTCOME_SUCCEEDED_WITH_READ_ERRORS,
  BRO_OUTCOME_SUCCEEDED_STEPS_FAILED,
  BRO_OUTCOME_FAILED,
  BRO_OUTCOME_CANCELLED,
  BRO_OUTCOME_SKIPPED,
  BRO_OUTCOME_INTERRUPTED,
} BroOutcome;

/* The wire form ("succeeded"), as in shared/schema/common.json. */
const char *bro_outcome_to_wire (BroOutcome value);
gboolean bro_outcome_from_wire (const char *text, BroOutcome *out);

G_END_DECLS
