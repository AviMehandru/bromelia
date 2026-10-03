/* bro-episode-plan.h: where the episodes of one title start and end: the first chapter of each episode (1-based),
 * the last chapter of the last one and the rule that found it, the chapters (at least 1 s) played after it, the
 * chapters to split at, the duration and chapter starts in seconds, the chapters kept, each episode's duration, and
 * how each start is reached. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int title;
  GArray *starts; /* int */
  int last_end;
  char *end_rule;
  GArray *tail;           /* int */
  GArray *split_chapters; /* int */
  double duration;
  GArray *chapter_starts; /* double */
  int kept_chapters;
  GArray *episode_durations; /* double */
  GStrv reasons;
} BroEpisodePlan;

void bro_episode_plan_free (BroEpisodePlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEpisodePlan, bro_episode_plan_free)

G_END_DECLS
