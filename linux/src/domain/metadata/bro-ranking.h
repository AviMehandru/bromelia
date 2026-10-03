/* bro-ranking.h: BroRanking, orders search results. */
#pragma once

#include "bro-candidate.h"

G_BEGIN_DECLS

/* Copies of @candidates (BroCandidate *), best first: the title matching @name (ignoring case and punctuation) scores
 * 4, the year 2 (1 when it is off by one; @year -1: none); ties keep the provider's order. */
GPtrArray *bro_ranking_rank (GPtrArray *candidates, const char *name, int year);

G_END_DECLS
