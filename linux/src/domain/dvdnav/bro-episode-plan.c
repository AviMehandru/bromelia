/* bro-episode-plan.c */
#include "bro-episode-plan.h"

void
bro_episode_plan_free (BroEpisodePlan *plan)
{
  if (!plan)
    return;
  g_array_unref (plan->starts);
  g_free (plan->end_rule);
  g_array_unref (plan->tail);
  g_array_unref (plan->split_chapters);
  g_array_unref (plan->chapter_starts);
  g_array_unref (plan->episode_durations);
  g_strfreev (plan->reasons);
  g_free (plan);
}
