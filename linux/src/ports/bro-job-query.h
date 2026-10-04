/* bro-job-query.h: which jobs: in a state, of a kind, finished after an instant; newest first, a page at a time. */
#pragma once

#include "bro-instant.h"
#include "bro-job-kind.h"
#include "bro-job-state.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gboolean has_state;
  BroJobState state;
  gboolean has_kind;
  BroJobKind kind;
  gboolean has_finished_after;
  BroInstant finished_after;
  int limit;
  int offset;
} BroJobQuery;

/* Everything zero. */
BroJobQuery *bro_job_query_new (void);
void bro_job_query_free (BroJobQuery *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobQuery, bro_job_query_free)

G_END_DECLS
