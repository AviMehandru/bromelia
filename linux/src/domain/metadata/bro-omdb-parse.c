/* bro-omdb-parse.c */
#include "bro-omdb-parse.h"

#include "bro-metadata-private.h"

#include <stdlib.h>
#include <string.h>

#define M bro_json_value_member

static BroCandidate *
match (BroJsonValue *r, gboolean has_kind, BroMediaKind kind)
{
  const char *title = bro_json_value_get_string (M (r, "Title"), NULL);
  if (!title || !*title)
    return NULL;
  BroCandidate *c = bro_candidate_new (title, "OMDb");
  c->year = _bro_metadata_year (M (r, "Year"));
  c->imdb_id = g_strdup (bro_json_value_get_string (M (r, "imdbID"), NULL));
  const char *type = bro_json_value_get_string (M (r, "Type"), "");
  c->has_kind = TRUE;
  if (strcmp (type, "series") == 0)
    c->kind = BRO_MEDIA_KIND_TV;
  else if (strcmp (type, "movie") == 0)
    c->kind = BRO_MEDIA_KIND_MOVIE;
  else {
    c->has_kind = has_kind;
    c->kind = kind;
  }
  g_free (c->overview);
  c->overview = g_strdup (_bro_metadata_text (M (r, "Plot")));
  g_free (c->poster);
  c->poster = g_strdup (_bro_metadata_text (M (r, "Poster")));
  return c;
}

/* The answer when OMDb says "Response": "True", else NULL. */
static BroJsonValue *
answer (GBytes *bytes)
{
  BroJsonValue *o = _bro_metadata_object (bytes);
  if (o && g_strcmp0 (bro_json_value_get_string (M (o, "Response"), NULL), "True") != 0)
    g_clear_pointer (&o, bro_json_value_unref);
  return o;
}

GPtrArray *
bro_omdb_parse_candidates (GBytes *bytes, gboolean has_kind, BroMediaKind kind)
{
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_candidate_free);
  g_autoptr (BroJsonValue) o = answer (bytes);
  if (!o)
    return list;
  BroJsonValue *search = M (o, "Search");
  if (search && search->kind == BRO_JSON_VALUE_ARRAY) {
    for (guint i = 0; i < search->items->len; i++) {
      BroJsonValue *r = search->items->pdata[i];
      BroCandidate *c = r->kind == BRO_JSON_VALUE_OBJECT ? match (r, has_kind, kind) : NULL;
      if (c)
        g_ptr_array_add (list, c);
    }
    return list;
  }
  BroCandidate *one = match (o, has_kind, kind);
  if (one)
    g_ptr_array_add (list, one);
  return list;
}

BroCandidate *
bro_omdb_parse_details (GBytes *bytes, gboolean has_kind, BroMediaKind kind)
{
  g_autoptr (BroJsonValue) o = answer (bytes);
  return o ? match (o, has_kind, kind) : NULL;
}

GHashTable *
bro_omdb_parse_season (GBytes *bytes)
{
  GHashTable *d = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_episode_details_free);
  g_autoptr (BroJsonValue) o = _bro_metadata_object (bytes);
  BroJsonValue *episodes = M (o, "Episodes");
  for (guint i = 0; i < bro_json_value_length (episodes); i++) {
    BroJsonValue *e = bro_json_value_at (episodes, i);
    const char *num = bro_json_value_get_string (M (e, "Episode"), "");
    const char *t = bro_json_value_get_string (M (e, "Title"), NULL);
    gboolean digits = *num && strlen (num) <= 9;
    for (const char *p = num; *p && digits; p++)
      digits = g_ascii_isdigit (*p);
    if (digits && t && *t)
      g_hash_table_replace (d, GINT_TO_POINTER (atoi (num)), bro_episode_details_new (t, _bro_metadata_text (M (e, "Released")), ""));
  }
  return d;
}
