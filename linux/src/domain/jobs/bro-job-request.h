/* bro-job-request.h: what a job was asked to do (stored as jobs.request). The kind-specific part (the session
 * snapshot of a hand-picked rip, a verify target, a command step) is in details; the engine's phase gives it
 * types. */
#pragma once

#include "bro-id.h"
#include "bro-job-kind.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  BroJobKind kind;
  char *title;
  gboolean automatic;
  char *drive_id;             /* nullable */
  gboolean has_media_generation;
  gint64 media_generation;
  BroId *session_id;          /* nullable */
  BroId *parent_id;           /* nullable */
  BroId *unit_id;             /* nullable */
  BroJsonValue *options;      /* nullable */
  BroJsonValue *details;      /* nullable */
} BroJobRequest;

BroJobRequest *bro_job_request_new (BroJobKind kind, const char *title);
void bro_job_request_free (BroJobRequest *request);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobRequest, bro_job_request_free)

G_END_DECLS
