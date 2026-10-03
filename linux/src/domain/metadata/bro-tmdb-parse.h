/* bro-tmdb-parse.h: BroTmdbParse, TMDb's answers. */
#pragma once

#include "bro-candidate.h"
#include "bro-episode-details.h"

G_BEGIN_DECLS

/* The results (BroCandidate *) of a search, in TMDb's order. */
GPtrArray *bro_tmdb_parse_candidates (GBytes *bytes, BroMediaKind kind);

/* A movie or show read by its id: details, or /find (the kind the id belongs to, whatever the disc was taken for;
 * @kind breaks a tie). NULL when the answer has none. */
BroCandidate *bro_tmdb_parse_details (GBytes *bytes, BroMediaKind kind);

/* Episode number (GINT_TO_POINTER) → BroEpisodeDetails *: title, air date and plot. */
GHashTable *bro_tmdb_parse_season (GBytes *bytes);

/* The id of the show's absolute-order episode group: the first group of type 2; NULL when there is none. */
char *bro_tmdb_parse_absolute_group (GBytes *bytes);

/* Absolute episode number (GINT_TO_POINTER) → BroEpisodeDetails *: an episode's place across the groups (groups by
 * order, episodes by order), counting from 1. */
GHashTable *bro_tmdb_parse_absolute_episodes (GBytes *bytes);

G_END_DECLS
