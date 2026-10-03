/* bro-episode-details.c */
#include "bro-episode-details.h"

BroEpisodeDetails *
bro_episode_details_new (const char *title, const char *aired, const char *plot)
{
  BroEpisodeDetails *d = g_new0 (BroEpisodeDetails, 1);
  d->title = g_strdup (title);
  d->aired = g_strdup (aired ? aired : "");
  d->plot = g_strdup (plot ? plot : "");
  return d;
}

void
bro_episode_details_free (BroEpisodeDetails *details)
{
  if (!details)
    return;
  g_free (details->title);
  g_free (details->aired);
  g_free (details->plot);
  g_free (details);
}
