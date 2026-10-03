/* bro-previous-episode.h: where a disc's episode numbering continues: the last episode before it, and where that
 * was found (episodes.how.previousDisc's source: a folder). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int last_episode;
  char *source;
} BroPreviousEpisode;

void bro_previous_episode_free (BroPreviousEpisode *previous);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroPreviousEpisode, bro_previous_episode_free)

G_END_DECLS
