/* bro-step-type.h: what a post-processing step runs */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_STEP_TYPE_COMMAND,
  BRO_STEP_TYPE_HANDBRAKE,
} BroStepType;

/* The wire form ("command"), as in shared/schema/common.json. */
const char *bro_step_type_to_wire (BroStepType value);
gboolean bro_step_type_from_wire (const char *text, BroStepType *out);

G_END_DECLS
