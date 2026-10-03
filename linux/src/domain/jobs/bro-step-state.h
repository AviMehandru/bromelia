/* bro-step-state.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_STEP_STATE_PENDING,
  BRO_STEP_STATE_RUNNING,
  BRO_STEP_STATE_SUCCEEDED,
  BRO_STEP_STATE_FAILED,
  BRO_STEP_STATE_SKIPPED,
  BRO_STEP_STATE_CANCELLED,
  BRO_STEP_STATE_INTERRUPTED,
} BroStepState;

/* The wire form ("pending"), as in shared/schema/common.json. */
const char *bro_step_state_to_wire (BroStepState value);
gboolean bro_step_state_from_wire (const char *text, BroStepState *out);

G_END_DECLS
