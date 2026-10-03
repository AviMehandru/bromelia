/* bro-job-state.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_JOB_STATE_QUEUED,
  BRO_JOB_STATE_WAITING_FOR_RESOURCES,
  BRO_JOB_STATE_RUNNING,
  BRO_JOB_STATE_BLOCKED,
  BRO_JOB_STATE_AWAITING_DECISION,
  BRO_JOB_STATE_FINISHED,
} BroJobState;

/* The wire form ("queued"), as in shared/schema/common.json. */
const char *bro_job_state_to_wire (BroJobState value);
gboolean bro_job_state_from_wire (const char *text, BroJobState *out);

G_END_DECLS
