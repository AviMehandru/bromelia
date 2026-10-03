/* bro-previous-episode.c */
#include "bro-previous-episode.h"

void
bro_previous_episode_free (BroPreviousEpisode *previous)
{
  if (!previous)
    return;
  g_free (previous->source);
  g_free (previous);
}
