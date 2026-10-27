/* bro-job-step-dto.h: BroJobStepDto: one step of a job's plan as JobDetail lists it (dtos.json JobDetail.steps): its
 * state, times, error and summary from the step's stored row; pending, with nothing else, when it has none. */
#pragma once

#include "bro-bro-error.h"
#include "bro-bro-message.h"
#include "bro-instant.h"
#include "bro-step-kind.h"
#include "bro-step-state.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int seq;
  BroStepKind kind;
  BroStepState state;
  gboolean has_started_at;
  BroInstant started_at;
  gboolean has_finished_at;
  BroInstant finished_at;
  BroBroError *error;     /* nullable */
  BroBroMessage *summary; /* nullable */
} BroJobStepDto;

/* No times, error or summary. */
BroJobStepDto *bro_job_step_dto_new (int seq, BroStepKind kind, BroStepState state);
void bro_job_step_dto_free (BroJobStepDto *step);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobStepDto, bro_job_step_dto_free)

G_END_DECLS
