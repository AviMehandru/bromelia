/* bro-omdb-parse.h: BroOmdbParse, OMDb's answers. @kind (with @has_kind) is used when a result doesn't say. */
#pragma once

#include "bro-candidate.h"
#include "bro-episode-details.h"

G_BEGIN_DECLS

/* The results (BroCandidate *) of a search (or the one title OMDb answered with), in OMDb's order. */
GPtrArray *bro_omdb_parse_candidates (GBytes *bytes, gboolean has_kind, BroMediaKind kind);

/* A movie or show read by its IMDb id; NULL when the answer has none. */
BroCandidate *bro_omdb_parse_details (GBytes *bytes, gboolean has_kind, BroMediaKind kind);

/* Episode number (GINT_TO_POINTER) → BroEpisodeDetails *: title and air date (OMDb lists no plots here). */
GHashTable *bro_omdb_parse_season (GBytes *bytes);

G_END_DECLS
