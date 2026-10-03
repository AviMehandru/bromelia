/* bro-menu-numbers.c */
#include "bro-menu-numbers.h"

#include <stdlib.h>

static gboolean
contains (GArray *a, int v)
{
  for (guint i = 0; i < a->len; i++)
    if (g_array_index (a, int, i) == v)
      return TRUE;
  return FALSE;
}

GArray *
bro_menu_numbers_parse (const char *text)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GRegex) re = g_regex_new ("EPIS[O0]DE\\s*#?\\s*([0-9]{1,4})\\b", G_REGEX_CASELESS, 0, NULL);
  g_autoptr (GMatchInfo) info = NULL;
  g_regex_match (re, text, 0, &info);
  while (g_match_info_matches (info)) {
    g_autofree char *n = g_match_info_fetch (info, 1);
    int v = atoi (n);
    if (!contains (out, v))
      g_array_append_val (out, v);
    g_match_info_next (info, NULL);
  }
  return out;
}

gboolean
bro_menu_numbers_first_episode (GArray *numbers, int count, int *out)
{
  g_autoptr (GArray) set = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < numbers->len; i++)
    if (!contains (set, g_array_index (numbers, int, i)))
      g_array_append_val (set, g_array_index (numbers, int, i));
  if (set->len == 0 || count <= 0)
    return FALSE;
  g_autoptr (GArray) starts = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < set->len; i++)
    for (int k = 0; k < count; k++) {
      int s = g_array_index (set, int, i) - k;
      if (s >= 0 && !contains (starts, s))
        g_array_append_val (starts, s);
    }
  int best = -1, winner = 0, winners = 0;
  for (guint i = 0; i < starts->len; i++) {
    int s = g_array_index (starts, int, i), score = 0;
    for (guint k = 0; k < set->len; k++) {
      int n = g_array_index (set, int, k);
      if (s <= n && n < s + count)
        score++;
    }
    if (score > best) {
      best = score;
      winner = s;
      winners = 1;
    } else if (score == best) {
      winners++;
    }
  }
  if (winners != 1 || best < 2)
    return FALSE;
  *out = winner;
  return TRUE;
}
