/* bro-step-output.h: what a step produced (stored as job_steps.output), for the steps after it. */
#pragma once

#include "bro-bro-message.h"
#include "bro-json-value.h"
#include "bro-step-kind.h"

G_BEGIN_DECLS

typedef struct {
  BroStepKind kind;
  BroJsonValue *data;
  BroBroMessage *summary; /* nullable */
} BroStepOutput;

/* Takes ownership of @data and @summary. */
BroStepOutput *bro_step_output_new (BroStepKind kind, BroJsonValue *data, BroBroMessage *summary);
void bro_step_output_free (BroStepOutput *output);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepOutput, bro_step_output_free)

G_END_DECLS
