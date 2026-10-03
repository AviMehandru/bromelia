/* bro-omdb-requests.h: BroOmdbRequests, OMDb requests (the key goes into the query). */
#pragma once

#include "bro-candidate.h"
#include "bro-http-request-spec.h"
#include "bro-online-id.h"

G_BEGIN_DECLS

/* The search for @name, optionally released in @year (-1: any). */
BroHttpRequestSpec *bro_omdb_requests_search (const char *name, BroMediaKind kind, int year, const char *key);

/* The movie or show with this IMDb id, with its full plot; NULL for a TMDb id (OMDb takes IMDb ids only). */
BroHttpRequestSpec *bro_omdb_requests_details (const BroOnlineId *id, const char *key);

/* The episodes of @season of the show @match (NULL without an IMDb id). */
BroHttpRequestSpec *bro_omdb_requests_season (const BroCandidate *match, int season, const char *key);

G_END_DECLS
