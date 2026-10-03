/* bro-one-pass.c */
#include "bro-one-pass.h"
#include "bro-listing-match.h"

/* Rips with fewer titles gain nothing: the extra listing costs as much as the runs it saves. */
#define MINIMUM_TITLES 3

static GHashTable *
set_of (GArray *ints)
{
  GHashTable *set = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint i = 0; i < ints->len; i++)
    g_hash_table_add (set, GINT_TO_POINTER (g_array_index (ints, int, i)));
  return set;
}

gboolean
bro_one_pass_plan (GArray *indices, const BroListing *listing, int current_min_length, BroOnePassPlan *out)
{
  g_autoptr (GHashTable) set = set_of (indices);
  guint n = g_hash_table_size (set);
  if (n < MINIMUM_TITLES || n >= listing->titles->len)
    return FALSE;
  guint picked = 0, others = 0;
  int shortest = 0, longest_other = 0;
  for (guint i = 0; i < listing->titles->len; i++) {
    const BroTitle *t = listing->titles->pdata[i];
    if (g_hash_table_contains (set, GINT_TO_POINTER (t->index))) {
      shortest = picked++ == 0 ? t->duration_seconds : MIN (shortest, t->duration_seconds);
    } else {
      longest_other = others++ == 0 ? t->duration_seconds : MAX (longest_other, t->duration_seconds);
    }
  }
  if (picked != n || others == 0 || shortest - longest_other < 2)
    return FALSE;
  int length = longest_other + 1;
  if (length <= MAX (current_min_length, 0))
    return FALSE;
  out->min_length = length;
  return TRUE;
}

gboolean
bro_one_pass_matches (const BroListing *listing, GArray *chosen, const BroListing *original)
{
  g_autoptr (GHashTable) set = set_of (chosen);
  if (listing->titles->len != g_hash_table_size (set))
    return FALSE;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GHashTable) map = bro_listing_match_map_titles (chosen, original, listing, NULL, &error);
  if (!map)
    return FALSE;
  g_autoptr (GHashTable) mapped = g_hash_table_new (g_direct_hash, g_direct_equal);
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init (&it, map);
  while (g_hash_table_iter_next (&it, NULL, &v))
    g_hash_table_add (mapped, v);
  if (g_hash_table_size (mapped) != listing->titles->len)
    return FALSE;
  for (guint i = 0; i < listing->titles->len; i++)
    if (!g_hash_table_contains (mapped, GINT_TO_POINTER (((BroTitle *) listing->titles->pdata[i])->index)))
      return FALSE;
  return TRUE;
}
