/* bro-kind-heuristics.h: BroKindHeuristics, whether a disc is a movie or a TV show. */
#pragma once

#include "bro-decision.h"
#include "bro-label.h"
#include "bro-listing.h"

G_BEGIN_DECLS

/* TV when the label has season / volume markers, the menu plays three or more episodes in one title, or three or
 * more titles (of the nullable @listing) are of episode length; a movie otherwise. */
BroDecision *bro_kind_heuristics_decide (const BroLabel *label, const BroListing *listing, int play_all_episodes);

/* The titles (BroTitle *, borrowed) of 10–75 minutes within ±35 % of the median of such titles (none when fewer
 * than two). */
GPtrArray *bro_kind_heuristics_episode_like (GPtrArray *titles);

G_END_DECLS
