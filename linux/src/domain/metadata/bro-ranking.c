/* bro-ranking.c */
#include "bro-ranking.h"

#include <stdlib.h>

/* Lower case, letters and digits only: "Dune: Part Two" → "duneparttwo". */
static char *
normalize (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; p && *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (g_unichar_isalpha (c) || g_unichar_isdigit (c))
      g_string_append_unichar (out, g_unichar_tolower (c));
  }
  return g_string_free (out, FALSE);
}

typedef struct {
  int score, index;
  BroCandidate *candidate;
} Scored;

static int
by_score (gconstpointer a, gconstpointer b)
{
  const Scored *x = a, *y = b;
  if (x->score != y->score)
    return x->score > y->score ? -1 : 1;
  return x->index < y->index ? -1 : x->index > y->index;
}

GPtrArray *
bro_ranking_rank (GPtrArray *candidates, const char *name, int year)
{
  g_autofree char *want = normalize (name);
  g_autoptr (GArray) scored = g_array_new (FALSE, FALSE, sizeof (Scored));
  for (guint i = 0; i < candidates->len; i++) {
    BroCandidate *c = candidates->pdata[i];
    g_autofree char *title = normalize (c->title);
    int score = g_strcmp0 (title, want) == 0 ? 4 : 0;
    if (year >= 0 && c->year >= 0)
      score += c->year == year ? 2 : abs (c->year - year) == 1 ? 1 : 0;
    Scored s = { score, (int) i, c };
    g_array_append_val (scored, s);
  }
  g_array_sort (scored, by_score);
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_candidate_free);
  for (guint i = 0; i < scored->len; i++)
    g_ptr_array_add (out, bro_candidate_copy (g_array_index (scored, Scored, i).candidate));
  return out;
}
