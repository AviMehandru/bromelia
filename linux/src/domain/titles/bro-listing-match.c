/* bro-listing-match.c */
#include "bro-listing-match.h"

#include <string.h>

static char *
key (const BroTitle *t)
{
  return g_strdup_printf ("%d|%d|%s", t->has_source_title_id ? t->source_title_id : -1, t->duration_seconds, t->segment_map);
}

BroBroMessage *
bro_listing_match_different_disc (const BroListing *old, const BroListing *new_listing)
{
  if (old->volume_name[0] && new_listing->volume_name[0] && strcmp (old->volume_name, new_listing->volume_name) != 0) {
    BroJsonValue *p = bro_json_value_new_object ();
    bro_json_value_set (p, "old", bro_json_value_new_string (old->volume_name));
    bro_json_value_set (p, "new", bro_json_value_new_string (new_listing->volume_name));
    return bro_bro_message_new (BRO_MSG_DISC_CHANGED_VOLUME, p, BRO_SEVERITY_INFO);
  }
  g_autoptr (GHashTable) a = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < old->titles->len; i++)
    g_hash_table_add (a, key (old->titles->pdata[i]));
  gboolean overlap = FALSE;
  for (guint i = 0; i < new_listing->titles->len && !overlap; i++) {
    g_autofree char *k = key (new_listing->titles->pdata[i]);
    overlap = g_hash_table_contains (a, k);
  }
  if (g_hash_table_size (a) > 0 && new_listing->titles->len > 0 && !overlap)
    return bro_bro_message_new (BRO_MSG_DISC_CHANGED_TITLES, NULL, BRO_SEVERITY_INFO);
  return NULL;
}

static int
compare_ints (gconstpointer a, gconstpointer b)
{
  int x = *(const int *) a, y = *(const int *) b;
  return x < y ? -1 : x > y;
}

static void
fail (BroBroError **error, BroMessageCode code, BroJsonValue *params)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, BRO_SEVERITY_ERROR);
  if (error)
    *error = bro_bro_message_to_error (m, NULL);
}

GHashTable *
bro_listing_match_map_titles (GArray *indices, const BroListing *old, const BroListing *new_listing, GHashTable *same_tracks,
                              BroBroError **error)
{
  g_autoptr (GArray) sorted = g_array_copy (indices);
  g_array_sort (sorted, compare_ints);
  g_autoptr (GHashTable) result = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_autoptr (GHashTable) used = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint n = 0; n < sorted->len; n++) {
    int i = g_array_index (sorted, int, n);
    if (n > 0 && g_array_index (sorted, int, n - 1) == i)
      continue;
    const BroTitle *t = NULL;
    for (guint k = 0; k < old->titles->len && !t; k++)
      if (((BroTitle *) old->titles->pdata[k])->index == i)
        t = old->titles->pdata[k];
    if (!t) {
      BroJsonValue *p = bro_json_value_new_object ();
      bro_json_value_set (p, "title", bro_json_value_new_integer (i));
      fail (error, BRO_MSG_DISC_TITLE_NOT_IN_LISTING, p);
      return NULL;
    }
    g_autofree char *tk = key (t);
    const BroTitle *same = NULL, *first = NULL;
    for (guint k = 0; k < new_listing->titles->len; k++) {
      const BroTitle *x = new_listing->titles->pdata[k];
      g_autofree char *xk = key (x);
      if (strcmp (xk, tk) != 0 || g_hash_table_contains (used, GINT_TO_POINTER (x->index)))
        continue;
      if (!first)
        first = x;
      if (x->index == i && !same)
        same = x;
    }
    const BroTitle *match = same ? same : first;
    if (!match) {
      BroJsonValue *p = bro_json_value_new_object ();
      bro_json_value_set (p, "title", bro_json_value_new_integer (i));
      bro_json_value_set (p, "duration", bro_json_value_new_string (t->duration));
      g_autofree char *source = t->source_file[0] ? g_strdup (t->source_file)
                                : t->has_source_title_id ? g_strdup_printf ("#%d", t->source_title_id) : g_strdup ("?");
      bro_json_value_set (p, "source", bro_json_value_new_string (source));
      fail (error, BRO_MSG_DISC_TITLE_GONE, p);
      return NULL;
    }
    if (same_tracks && g_hash_table_contains (same_tracks, GINT_TO_POINTER (i))) {
      gboolean equal = t->tracks->len == match->tracks->len;
      for (guint k = 0; equal && k < t->tracks->len; k++)
        equal = ((BroTrack *) t->tracks->pdata[k])->kind == ((BroTrack *) match->tracks->pdata[k])->kind;
      if (!equal) {
        BroJsonValue *p = bro_json_value_new_object ();
        bro_json_value_set (p, "title", bro_json_value_new_integer (i));
        fail (error, BRO_MSG_DISC_TRACKS_CHANGED, p);
        return NULL;
      }
    }
    g_hash_table_insert (result, GINT_TO_POINTER (i), GINT_TO_POINTER (match->index));
    g_hash_table_add (used, GINT_TO_POINTER (match->index));
  }
  return g_steal_pointer (&result);
}
