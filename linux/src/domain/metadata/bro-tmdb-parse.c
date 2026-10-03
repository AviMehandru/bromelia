/* bro-tmdb-parse.c */
#include "bro-tmdb-parse.h"

#include "bro-metadata-private.h"

#define M bro_json_value_member

static GHashTable *
episodes_table (void)
{
  return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_episode_details_free);
}

static BroCandidate *
match (BroJsonValue *r, BroMediaKind kind)
{
  const char *title = bro_json_value_get_string (M (r, "title"), NULL);
  if (!title)
    title = bro_json_value_get_string (M (r, "name"), NULL);
  if (!title || !*title)
    return NULL;
  BroCandidate *c = bro_candidate_new (title, "TMDb");
  BroJsonValue *date = M (r, "release_date");
  c->year = _bro_metadata_year (date ? date : M (r, "first_air_date"));
  _bro_metadata_int (M (r, "id"), &c->tmdb_id);
  const char *imdb = _bro_metadata_text (M (r, "imdb_id"));
  c->imdb_id = *imdb ? g_strdup (imdb) : NULL;
  c->has_kind = TRUE;
  c->kind = kind;
  g_free (c->overview);
  c->overview = g_strdup (_bro_metadata_text (M (r, "overview")));
  const char *poster = bro_json_value_get_string (M (r, "poster_path"), NULL);
  if (poster) {
    g_free (c->poster);
    c->poster = g_strconcat ("https://image.tmdb.org/t/p/original", poster, NULL);
  }
  return c;
}

static BroJsonValue *
first_object (BroJsonValue *list)
{
  for (guint i = 0; i < bro_json_value_length (list); i++)
    if (bro_json_value_at (list, i)->kind == BRO_JSON_VALUE_OBJECT)
      return bro_json_value_at (list, i);
  return NULL;
}

GPtrArray *
bro_tmdb_parse_candidates (GBytes *bytes, BroMediaKind kind)
{
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_candidate_free);
  g_autoptr (BroJsonValue) o = _bro_metadata_object (bytes);
  BroJsonValue *results = M (o, "results");
  for (guint i = 0; i < bro_json_value_length (results); i++) {
    BroJsonValue *r = bro_json_value_at (results, i);
    BroCandidate *c = r->kind == BRO_JSON_VALUE_OBJECT ? match (r, kind) : NULL;
    if (c)
      g_ptr_array_add (list, c);
  }
  return list;
}

BroCandidate *
bro_tmdb_parse_details (GBytes *bytes, BroMediaKind kind)
{
  g_autoptr (BroJsonValue) o = _bro_metadata_object (bytes);
  if (!o)
    return NULL;
  if (M (o, "movie_results") || M (o, "tv_results")) {
    BroJsonValue *movie = first_object (M (o, "movie_results")), *show = first_object (M (o, "tv_results"));
    if (kind == BRO_MEDIA_KIND_TV)
      return show ? match (show, BRO_MEDIA_KIND_TV) : movie ? match (movie, BRO_MEDIA_KIND_MOVIE) : NULL;
    return movie ? match (movie, BRO_MEDIA_KIND_MOVIE) : show ? match (show, BRO_MEDIA_KIND_TV) : NULL;
  }
  if (!M (o, "id"))
    return NULL;
  return match (o, M (o, "name") && !M (o, "title") ? BRO_MEDIA_KIND_TV : BRO_MEDIA_KIND_MOVIE);
}

static void
put_episode (GHashTable *d, int n, BroJsonValue *e)
{
  const char *t = bro_json_value_get_string (M (e, "name"), NULL);
  if (t && *t)
    g_hash_table_replace (d, GINT_TO_POINTER (n),
                          bro_episode_details_new (t, _bro_metadata_text (M (e, "air_date")), _bro_metadata_text (M (e, "overview"))));
}

GHashTable *
bro_tmdb_parse_season (GBytes *bytes)
{
  GHashTable *d = episodes_table ();
  g_autoptr (BroJsonValue) o = _bro_metadata_object (bytes);
  BroJsonValue *episodes = M (o, "episodes");
  for (guint i = 0; i < bro_json_value_length (episodes); i++) {
    BroJsonValue *e = bro_json_value_at (episodes, i);
    int n;
    if (e->kind == BRO_JSON_VALUE_OBJECT && _bro_metadata_int (M (e, "episode_number"), &n))
      put_episode (d, n, e);
  }
  return d;
}

char *
bro_tmdb_parse_absolute_group (GBytes *bytes)
{
  g_autoptr (BroJsonValue) o = _bro_metadata_object (bytes);
  BroJsonValue *results = M (o, "results");
  for (guint i = 0; i < bro_json_value_length (results); i++) {
    BroJsonValue *g = bro_json_value_at (results, i);
    int type;
    if (_bro_metadata_int (M (g, "type"), &type) && type == 2)
      return g_strdup (bro_json_value_get_string (M (g, "id"), NULL));
  }
  return NULL;
}

typedef struct {
  int order, index;
  BroJsonValue *value;
} Ordered;

static int
by_order (gconstpointer a, gconstpointer b)
{
  const Ordered *x = a, *y = b;
  if (x->order != y->order)
    return x->order < y->order ? -1 : 1;
  return x->index < y->index ? -1 : x->index > y->index;
}

/* The objects of @list sorted by their "order" (then by place). */
static GArray *
ordered (BroJsonValue *list)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (Ordered));
  for (guint i = 0; i < bro_json_value_length (list); i++) {
    BroJsonValue *v = bro_json_value_at (list, i);
    if (v->kind != BRO_JSON_VALUE_OBJECT)
      continue;
    Ordered o = { 0, (int) i, v };
    _bro_metadata_int (M (v, "order"), &o.order);
    g_array_append_val (a, o);
  }
  g_array_sort (a, by_order);
  return a;
}

GHashTable *
bro_tmdb_parse_absolute_episodes (GBytes *bytes)
{
  GHashTable *d = episodes_table ();
  g_autoptr (BroJsonValue) o = _bro_metadata_object (bytes);
  g_autoptr (GArray) groups = ordered (M (o, "groups"));
  int n = 0;
  for (guint i = 0; i < groups->len; i++) {
    g_autoptr (GArray) episodes = ordered (M (g_array_index (groups, Ordered, i).value, "episodes"));
    for (guint k = 0; k < episodes->len; k++)
      put_episode (d, ++n, g_array_index (episodes, Ordered, k).value);
  }
  return d;
}
