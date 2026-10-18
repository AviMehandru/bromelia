/* bro-job-query.h: which jobs (GET /v1/jobs, owner 2026-10-17): in any of the states, with any of the outcomes (an empty
 * list is any), of a kind, in a queue, on a drive, for a unit, finished after an instant; newest first, a page at a
 * time. */
#pragma once

#include "bro-id.h"
#include "bro-instant.h"
#include "bro-job-kind.h"
#include "bro-job-state.h"
#include "bro-outcome.h"
#include "bro-queue.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  GArray *states;   /* BroJobState */
  GArray *outcomes; /* BroOutcome */
  gboolean has_kind;
  BroJobKind kind;
  gboolean has_queue;
  BroQueue queue;
  char *drive_id; /* nullable */
  gboolean has_unit_id;
  BroId unit_id;
  gboolean has_finished_after;
  BroInstant finished_after;
  int limit;
  int offset;
} BroJobQuery;

/* Everything zero, the lists empty. */
BroJobQuery *bro_job_query_new (void);
void bro_job_query_free (BroJobQuery *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobQuery, bro_job_query_free)

G_END_DECLS
