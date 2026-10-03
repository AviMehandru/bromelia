/* bro-episode-details.h: an episode of a season, as listed online: its title, air date (yyyy-mm-dd or "") and plot. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *title;
  char *aired;
  char *plot;
} BroEpisodeDetails;

BroEpisodeDetails *bro_episode_details_new (const char *title, const char *aired, const char *plot);
void bro_episode_details_free (BroEpisodeDetails *details);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEpisodeDetails, bro_episode_details_free)

G_END_DECLS
