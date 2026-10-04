/* bro-job-record.h: A row of jobs. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-job-kind.h"
#include "bro-job-plan.h"
#include "bro-job-request.h"
#include "bro-job-state.h"
#include "bro-json-value.h"
#include "bro-outcome.h"
#include "bro-queue.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  BroJobKind kind;
  gboolean has_parent_id;
  BroId parent_id;
  BroJobState state;
  gboolean has_outcome;
  BroOutcome outcome;
  BroBroError *error; /* nullable */
  BroJsonValue *blocked_by; /* nullable */
  BroJsonValue *decision; /* nullable */
  BroQueue queue;
  int position;
  char *drive_id; /* nullable */
  gboolean has_media_generation;
  gint64 media_generation;
  gboolean automatic;
  char *mode; /* nullable */
  char *title;
  char *fingerprint; /* nullable */
  BroJobRequest *request;
  BroJobPlan *plan; /* nullable */
  gboolean has_unit_id;
  BroId unit_id;
  BroInstant created_at;
  gboolean has_started_at;
  BroInstant started_at;
  gboolean has_finished_at;
  BroInstant finished_at;
} BroJobRecord;

/* Everything zero. */
BroJobRecord *bro_job_record_new (void);
void bro_job_record_free (BroJobRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobRecord, bro_job_record_free)

G_END_DECLS
