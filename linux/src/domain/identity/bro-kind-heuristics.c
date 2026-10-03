/* bro-kind-heuristics.c */
#include "bro-kind-heuristics.h"

#include <math.h>
#include <stdlib.h>

static int
compare_ints (gconstpointer a, gconstpointer b)
{
  int x = *(const int *) a, y = *(const int *) b;
  return x < y ? -1 : x > y;
}

GPtrArray *
bro_kind_heuristics_episode_like (GPtrArray *titles)
{
  GPtrArray *out = g_ptr_array_new ();
  g_autoptr (GPtrArray) candidates = g_ptr_array_new ();
  for (guint i = 0; titles && i < titles->len; i++) {
    const BroTitle *t = titles->pdata[i];
    if (t->duration_seconds >= 600 && t->duration_seconds <= 4500)
      g_ptr_array_add (candidates, (gpointer) t);
  }
  if (candidates->len < 2)
    return out;
  g_autoptr (GArray) sorted = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < candidates->len; i++)
    g_array_append_val (sorted, ((BroTitle *) candidates->pdata[i])->duration_seconds);
  g_array_sort (sorted, compare_ints);
  double median = g_array_index (sorted, int, sorted->len / 2);
  for (guint i = 0; i < candidates->len; i++) {
    const BroTitle *t = candidates->pdata[i];
    if (fabs (t->duration_seconds - median) <= median * 0.35)
      g_ptr_array_add (out, (gpointer) t);
  }
  return out;
}

static BroDecision *
decision (BroMediaKind kind, BroMessageCode code, BroJsonValue *params)
{
  BroDecision *d = g_new0 (BroDecision, 1);
  d->value = kind;
  d->reason = bro_bro_message_new (code, params, BRO_SEVERITY_INFO);
  return d;
}

BroDecision *
bro_kind_heuristics_decide (const BroLabel *label, const BroListing *listing, int play_all_episodes)
{
  if (label->looks_like_series)
    return decision (BRO_MEDIA_KIND_TV, BRO_MSG_IDENTITY_REASON_LABEL_MARKERS, NULL);
  if (play_all_episodes >= 3) {
    BroJsonValue *p = bro_json_value_new_object ();
    bro_json_value_set (p, "count", bro_json_value_new_integer (play_all_episodes));
    return decision (BRO_MEDIA_KIND_TV, BRO_MSG_IDENTITY_REASON_MENU_EPISODES, p);
  }
  g_autoptr (GPtrArray) like = bro_kind_heuristics_episode_like (listing ? listing->titles : NULL);
  if (like->len >= 3)
    return decision (BRO_MEDIA_KIND_TV, BRO_MSG_IDENTITY_REASON_EPISODE_TITLES, NULL);
  return decision (BRO_MEDIA_KIND_MOVIE, BRO_MSG_IDENTITY_REASON_NONE, NULL);
}
