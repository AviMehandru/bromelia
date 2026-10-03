/* bro-tmdb-requests.h: BroTmdbRequests, TMDb requests. A v3 API key (40 characters or fewer) goes into the query; a
 * longer key is a read access token, sent as a Bearer header. @language "" is en-US. */
#pragma once

#include "bro-candidate.h"
#include "bro-http-request-spec.h"

G_BEGIN_DECLS

/* The search for @name, optionally released (or first aired) in @year (-1: any). */
BroHttpRequestSpec *bro_tmdb_requests_search (const char *name, BroMediaKind kind, int year, const char *key, const char *language);

/* The movie or show with TMDb id @id. */
BroHttpRequestSpec *bro_tmdb_requests_details (int id, BroMediaKind kind, const char *key, const char *language);

/* The movie or show with IMDb id @imdb_id. */
BroHttpRequestSpec *bro_tmdb_requests_find (const char *imdb_id, const char *key, const char *language);

/* The episodes of @season of the show @match (NULL without a TMDb id). */
BroHttpRequestSpec *bro_tmdb_requests_season (const BroCandidate *match, int season, const char *key, const char *language);

/* The episode groups of a show (one of them may be the absolute order). */
BroHttpRequestSpec *bro_tmdb_requests_episode_groups (int tmdb_id, const char *key, const char *language);

/* The episodes of an episode group. */
BroHttpRequestSpec *bro_tmdb_requests_episode_group (const char *group_id, const char *key, const char *language);

G_END_DECLS
