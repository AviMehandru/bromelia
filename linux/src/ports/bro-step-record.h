/* bro-step-record.h: A row of job_steps: the durable checkpoint between steps. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-json-value.h"
#include "bro-step-kind.h"
#include "bro-step-output.h"
#include "bro-step-state.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId job_id;
  int seq;
  BroStepKind kind;
  BroStepState state;
  int attempt;
  gboolean has_started_at;
  BroInstant started_at;
  gboolean has_finished_at;
  BroInstant finished_at;
  BroStepOutput *output; /* nullable */
  BroBroError *error; /* nullable */
  BroJsonValue *checkpoint; /* nullable */
} BroStepRecord;

/* Everything zero. */
BroStepRecord *bro_step_record_new (void);
void bro_step_record_free (BroStepRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepRecord, bro_step_record_free)

G_END_DECLS
