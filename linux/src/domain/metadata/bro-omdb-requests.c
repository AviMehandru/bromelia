/* bro-omdb-requests.c */
#include "bro-omdb-requests.h"

#include "bro-metadata-private.h"

static BroHttpRequestSpec *
get (const char *query, const char *key)
{
  g_autofree char *k = g_strstrip (g_strdup (key));
  g_autofree char *ek = _bro_metadata_escape (k);
  g_autofree char *url = g_strdup_printf ("https://www.omdbapi.com/?apikey=%s&%s", ek, query);
  return bro_http_request_spec_new ("GET", url);
}

BroHttpRequestSpec *
bro_omdb_requests_search (const char *name, BroMediaKind kind, int year, const char *key)
{
  g_autofree char *q = _bro_metadata_escape (name);
  const char *type = kind == BRO_MEDIA_KIND_TV ? "series" : "movie";
  g_autofree char *query = year >= 0 ? g_strdup_printf ("s=%s&type=%s&y=%d", q, type, year) : g_strdup_printf ("s=%s&type=%s", q, type);
  return get (query, key);
}

BroHttpRequestSpec *
bro_omdb_requests_details (const BroOnlineId *id, const char *key)
{
  if (id->kind != BRO_ONLINE_ID_IMDB)
    return NULL;
  g_autofree char *tt = _bro_metadata_escape (id->imdb_id);
  g_autofree char *query = g_strdup_printf ("i=%s&plot=full", tt);
  return get (query, key);
}

BroHttpRequestSpec *
bro_omdb_requests_season (const BroCandidate *match, int season, const char *key)
{
  if (!match->imdb_id)
    return NULL;
  g_autofree char *tt = _bro_metadata_escape (match->imdb_id);
  g_autofree char *query = g_strdup_printf ("i=%s&Season=%d", tt, season);
  return get (query, key);
}
