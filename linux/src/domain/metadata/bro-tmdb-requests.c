/* bro-tmdb-requests.c */
#include "bro-tmdb-requests.h"

#include "bro-metadata-private.h"

#include <string.h>

static BroHttpRequestSpec *
get (const char *path, const char *query, const char *key, const char *language)
{
  g_autofree char *k = g_strstrip (g_strdup (key));
  g_autofree char *lang = _bro_metadata_escape (language && *language ? language : "en-US");
  g_autofree char *url = g_strdup_printf ("https://api.themoviedb.org/3/%s?%s%slanguage=%s", path, query, *query ? "&" : "", lang);
  if (g_utf8_strlen (k, -1) <= 40) {
    g_autofree char *ek = _bro_metadata_escape (k);
    g_autofree char *with_key = g_strconcat (url, "&api_key=", ek, NULL);
    return bro_http_request_spec_new ("GET", with_key);
  }
  BroHttpRequestSpec *spec = bro_http_request_spec_new ("GET", url);
  g_autofree char *bearer = g_strconcat ("Bearer ", k, NULL);
  bro_http_request_spec_add_header (spec, "Authorization", bearer);
  return spec;
}

BroHttpRequestSpec *
bro_tmdb_requests_search (const char *name, BroMediaKind kind, int year, const char *key, const char *language)
{
  gboolean tv = kind == BRO_MEDIA_KIND_TV;
  g_autofree char *q = _bro_metadata_escape (name);
  g_autofree char *query = year >= 0 ? g_strdup_printf ("query=%s&%s=%d", q, tv ? "first_air_date_year" : "year", year) : g_strdup_printf ("query=%s", q);
  return get (tv ? "search/tv" : "search/movie", query, key, language);
}

BroHttpRequestSpec *
bro_tmdb_requests_details (int id, BroMediaKind kind, const char *key, const char *language)
{
  g_autofree char *path = g_strdup_printf ("%s/%d", kind == BRO_MEDIA_KIND_TV ? "tv" : "movie", id);
  return get (path, "", key, language);
}

BroHttpRequestSpec *
bro_tmdb_requests_find (const char *imdb_id, const char *key, const char *language)
{
  g_autofree char *e = _bro_metadata_escape (imdb_id);
  g_autofree char *path = g_strconcat ("find/", e, NULL);
  return get (path, "external_source=imdb_id", key, language);
}

BroHttpRequestSpec *
bro_tmdb_requests_season (const BroCandidate *match, int season, const char *key, const char *language)
{
  if (match->tmdb_id < 0)
    return NULL;
  g_autofree char *path = g_strdup_printf ("tv/%d/season/%d", match->tmdb_id, season);
  return get (path, "", key, language);
}

BroHttpRequestSpec *
bro_tmdb_requests_episode_groups (int tmdb_id, const char *key, const char *language)
{
  g_autofree char *path = g_strdup_printf ("tv/%d/episode_groups", tmdb_id);
  return get (path, "", key, language);
}

BroHttpRequestSpec *
bro_tmdb_requests_episode_group (const char *group_id, const char *key, const char *language)
{
  g_autofree char *e = _bro_metadata_escape (group_id);
  g_autofree char *path = g_strconcat ("tv/episode_group/", e, NULL);
  return get (path, "", key, language);
}
