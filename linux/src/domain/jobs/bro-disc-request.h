/* bro-disc-request.h: the details of a video-disc job's request (BroJobRequest.details): its source and, when the
 * titles were chosen on an opened disc, that session's snapshot. */
#pragma once

#include "bro-disc-source.h"
#include "bro-job-request.h"
#include "bro-session-snapshot.h"

G_BEGIN_DECLS

typedef struct {
  BroDiscSource *source;
  BroSessionSnapshot *session; /* nullable */
} BroDiscRequest;

/* Takes ownership of both. */
BroDiscRequest *bro_disc_request_new (BroDiscSource *source, BroSessionSnapshot *session);
void bro_disc_request_free (BroDiscRequest *request);

/* {"source": …, "session": {"listing": …, "choices": …}}; session left out when there is none. */
BroJsonValue *bro_disc_request_encode (const BroDiscRequest *request);

/* The request's details: the source they name, else the request's drive; NULL when there is neither. */
BroDiscRequest *bro_disc_request_decode (const BroJobRequest *request);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDiscRequest, bro_disc_request_free)

G_END_DECLS
