/* bro-nfo.h: BroNfo, Kodi / Jellyfin / Emby .nfo documents, written next to a media server library (was
 * MediaServerMetadata). */
#pragma once

#include "bro-candidate.h"
#include "bro-episode-details.h"

G_BEGIN_DECLS

/* movie.nfo: title, year, plot and the ids (the first one is the default). */
char *bro_nfo_movie (const BroCandidate *match);

/* tvshow.nfo: as for a movie. */
char *bro_nfo_show (const BroCandidate *match);

/* An episode's .nfo: title, show, season, episode, plot and air date. */
char *bro_nfo_episode (const char *show, int season, int episode, const BroEpisodeDetails *details);

G_END_DECLS
