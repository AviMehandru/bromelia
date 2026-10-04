/* bro-episode-continuation.h: BroEpisodeContinuation, where a TV disc's episode numbering continues: after the
 * last episode of the previous disc of its set (same show, season, part and volume; disc number one lower), or, in
 * a media server library, after the highest episode already in the season folder. */
#pragma once

#include "bro-archived-disc.h"
#include "bro-continuation-query.h"
#include "bro-previous-episode.h"

G_BEGIN_DECLS

/* The previous disc's record (@candidates: BroArchivedDisc *) when there is one; else the season folder's highest
 * episode, unless a later disc of the set was archived (the discs were ripped out of order). NULL for the first
 * disc. */
BroPreviousEpisode *bro_episode_continuation_choose (const BroContinuationQuery *query, GPtrArray *candidates);

/* Whether an archived disc is of the query's set: the same show (its name or its label's title, normalised) and the
 * same season, part and volume. */
gboolean bro_episode_continuation_same_set (const BroArchivedDisc *r, const BroContinuationQuery *q);

/* The highest episode number of @season in @file_names (… S02E05 …); FALSE when none. */
gboolean bro_episode_continuation_highest_in_season (const char *const *file_names, int season, int *out);

G_END_DECLS
