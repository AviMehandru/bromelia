/* bro-continuation-query.c */
#include "bro-continuation-query.h"

BroContinuationQuery *
bro_continuation_query_new (const char *name, const char *label_title, int disc)
{
  BroContinuationQuery *q = g_new0 (BroContinuationQuery, 1);
  q->name = g_strdup (name ? name : "");
  q->label_title = g_strdup (label_title ? label_title : "");
  q->season = q->part = q->volume = -1;
  q->disc = disc;
  q->season_folder_highest = -1;
  return q;
}

void
bro_continuation_query_free (BroContinuationQuery *q)
{
  if (!q)
    return;
  g_free (q->name);
  g_free (q->label_title);
  g_free (q->season_folder);
  g_free (q);
}
