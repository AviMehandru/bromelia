/* bro-job-dto.h: a job, as clients see it (dtos.json Job). Optional members are left out of the JSON when absent. */
#pragma once

#include "bro-bro-error.h"
#include "bro-decision-dto.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-job-kind.h"
#include "bro-job-record.h"
#include "bro-job-state.h"
#include "bro-job-totals.h"
#include "bro-outcome.h"
#include "bro-progress-dto.h"
#include "bro-queue.h"

G_BEGIN_DECLS

typedef struct {
  BroId id;
  BroJobKind kind;
  BroJobState state;
  gboolean has_outcome;
  BroOutcome outcome;
  BroQueue queue;
  gboolean has_position;
  int position;
  gboolean has_parent_id;
  BroId parent_id;
  char *drive_id;     /* nullable */
  BroId *session_id;  /* nullable */
  char *title;
  char *mode;         /* nullable */
  gboolean automatic;
  BroInstant created_at;
  gboolean has_started_at;
  BroInstant started_at;
  gboolean has_finished_at;
  BroInstant finished_at;
  BroProgressDto *progress;   /* nullable */
  BroBroMessage *blocked_by;  /* nullable */
  BroDecisionDto *decision;   /* nullable */
  BroBroError *error;         /* nullable */
  gboolean has_unit_id;
  BroId unit_id;
  char *output_path;  /* nullable */
  char *fingerprint;  /* nullable */
  BroJobTotals *counts; /* nullable */
} BroJobDto;

/* Everything zero; the owned members are set by the caller. */
BroJobDto *bro_job_dto_new (void);
void bro_job_dto_free (BroJobDto *job);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_job_dto_json (const BroJobDto *job);

/* The stored job as clients see it, without what only a running job has (progress, blockedBy, decision, position,
 * counts). */
BroJobDto *bro_job_dto_of (const BroJobRecord *record);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobDto, bro_job_dto_free)

G_END_DECLS
