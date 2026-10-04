/* bro-metadata-client.c */
#include "bro-metadata-client.h"

#include "bro-message-code.h"
#include "bro-omdb-parse.h"
#include "bro-omdb-requests.h"
#include "bro-ranking.h"
#include "bro-tmdb-parse.h"
#include "bro-tmdb-requests.h"
#include <string.h>

#define CACHE_MILLISECONDS ((gint64) 7 * 24 * 3600 * 1000)

struct _BroMetadataClient {
  GObject parent_instance;
  BroHttpClient *http;
  BroLookupCacheRepository *cache;
  BroClock *clock;
  BroFileSystem *fs;
  gboolean omdb;
  char *key;
  char *language;
};

G_DEFINE_FINAL_TYPE (BroMetadataClient, bro_metadata_client, G_TYPE_OBJECT)

static const char *
provider_name (BroMetadataClient *self)
{
  return self->omdb ? "OMDb" : "TMDb";
}

static void
set_http_error (BroMetadataClient *self, int status, BroBroError **error)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "provider", bro_json_value_new_string (provider_name (self)));
  bro_json_value_set (params, "status", bro_json_value_new_integer (status));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_METADATA_HTTP), params);
}

/* The URL without its api_key / apikey parameter. */
static char *
without_key (const char *url)
{
  const char *q = strchr (url, '?');
  g_auto (GStrv) params = NULL;
  GString *out;
  gboolean first = TRUE;
  if (!q)
    return g_strdup (url);
  out = g_string_new_len (url, q - url);
  params = g_strsplit (q + 1, "&", -1);
  for (guint i = 0; params[i]; i++)
    {
      if (g_str_has_prefix (params[i], "api_key=") || g_str_has_prefix (params[i], "apikey="))
        continue;
      g_string_append_c (out, first ? '?' : '&');
      g_string_append (out, params[i]);
      first = FALSE;
    }
  return g_string_free (out, FALSE);
}

/* The body of a 200 answer (from the cache when it is there). Takes @request. */
static GBytes *
fetch (BroMetadataClient *self, BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  const char *provider = self->omdb ? "omdb" : "tmdb";
  g_autofree char *bare = without_key (request->url);
  g_autofree char *key = g_strconcat (request->method, " ", bare, NULL);
  g_autoptr (BroHttpResponse) response = NULL;
  GBytes *body = NULL;
  if (self->cache)
    {
      g_autoptr (BroHttpResponse) cached = bro_lookup_cache_repository_get (self->cache, provider, key, NULL);
      if (cached)
        {
          bro_http_request_spec_free (request);
          return g_bytes_ref (cached->body);
        }
    }
  response = bro_http_client_send (self->http, request, cancel, error);
  bro_http_request_spec_free (request);
  if (!response)
    return NULL;
  if (response->status != 200)
    {
      set_http_error (self, response->status, error);
      return NULL;
    }
  if (self->cache)
    {
      BroInstant expires = { bro_clock_now (self->clock).unix_milliseconds + CACHE_MILLISECONDS };
      bro_lookup_cache_repository_put (self->cache, provider, key, response, expires, NULL);
    }
  body = g_bytes_ref (response->body);
  return body;
}

static GPtrArray *
find (BroMetadataClient *self, const char *name, BroMediaKind kind, int year, int rank_year, BroCancellationToken *cancel, BroBroError **error)
{
  BroHttpRequestSpec *r = self->omdb ? bro_omdb_requests_search (name, kind, year, self->key)
                                     : bro_tmdb_requests_search (name, kind, year, self->key, self->language);
  g_autoptr (GBytes) body = fetch (self, r, cancel, error);
  g_autoptr (GPtrArray) all = NULL;
  if (!body)
    return NULL;
  all = self->omdb ? bro_omdb_parse_candidates (body, TRUE, kind) : bro_tmdb_parse_candidates (body, kind);
  return bro_ranking_rank (all, name, rank_year);
}

GPtrArray *
bro_metadata_client_search (BroMetadataClient *self, const char *name, BroMediaKind kind, int year, BroCancellationToken *cancel,
                            BroBroError **error)
{
  GPtrArray *list = find (self, name, kind, year, year, cancel, error);
  if (list && list->len == 0 && year >= 0)
    {
      g_ptr_array_unref (list);
      list = find (self, name, kind, -1, year, cancel, error); /* ranked by the year asked for */
    }
  if (list && self->omdb && list->len > 0 && ((BroCandidate *) list->pdata[0])->imdb_id)
    {
      BroOnlineId id = { BRO_ONLINE_ID_IMDB, -1, FALSE, 0, ((BroCandidate *) list->pdata[0])->imdb_id };
      BroHttpRequestSpec *r = bro_omdb_requests_details (&id, self->key);
      g_autoptr (GBytes) body = r ? fetch (self, r, cancel, error) : NULL;
      BroCandidate *full = body ? bro_omdb_parse_details (body, TRUE, kind) : NULL;
      if (r && !body)
        {
          g_ptr_array_unref (list);
          return NULL;
        }
      if (full)
        {
          bro_candidate_free (list->pdata[0]);
          list->pdata[0] = full;
        }
    }
  return list;
}

static char *
shown (const BroOnlineId *id)
{
  if (id->kind == BRO_ONLINE_ID_IMDB)
    return g_strdup (id->imdb_id);
  if (id->has_media_kind)
    return g_strdup_printf ("%s/%d", id->media_kind == BRO_MEDIA_KIND_TV ? "tv" : "movie", id->tmdb_id);
  return g_strdup_printf ("%d", id->tmdb_id);
}

BroCandidate *
bro_metadata_client_lookup (BroMetadataClient *self, const BroOnlineId *id, BroMediaKind kind, BroCancellationToken *cancel, BroBroError **error)
{
  g_autoptr (GBytes) body = NULL;
  if (self->omdb)
    {
      BroHttpRequestSpec *r = bro_omdb_requests_details (id, self->key);
      if (!r)
        {
          g_autofree char *text = shown (id);
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "provider", bro_json_value_new_string (provider_name (self)));
          bro_json_value_set (params, "id", bro_json_value_new_string (text));
          bro_json_value_set (params, "omdb", bro_json_value_new_bool (TRUE));
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_METADATA_ID_UNSUPPORTED), params);
          return NULL;
        }
      body = fetch (self, r, cancel, error);
      return body ? bro_omdb_parse_details (body, TRUE, kind) : NULL;
    }
  if (id->kind == BRO_ONLINE_ID_TMDB)
    {
      BroMediaKind k = id->has_media_kind ? id->media_kind : kind;
      body = fetch (self, bro_tmdb_requests_details (id->tmdb_id, k, self->key, self->language), cancel, error);
      return body ? bro_tmdb_parse_details (body, k) : NULL;
    }
  body = fetch (self, bro_tmdb_requests_find (id->imdb_id, self->key, self->language), cancel, error);
  return body ? bro_tmdb_parse_details (body, kind) : NULL;
}

static GHashTable *
empty_episodes (void)
{
  return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_episode_details_free);
}

GHashTable *
bro_metadata_client_season (BroMetadataClient *self, const BroCandidate *match, int season, BroCancellationToken *cancel, BroBroError **error)
{
  BroHttpRequestSpec *r = self->omdb ? bro_omdb_requests_season (match, season, self->key)
                                     : bro_tmdb_requests_season (match, season, self->key, self->language);
  g_autoptr (GBytes) body = NULL;
  if (!r)
    return empty_episodes ();
  body = fetch (self, r, cancel, error);
  if (!body)
    return NULL;
  return self->omdb ? bro_omdb_parse_season (body) : bro_tmdb_parse_season (body);
}

GHashTable *
bro_metadata_client_absolute_episodes (BroMetadataClient *self, const BroCandidate *match, BroCancellationToken *cancel, BroBroError **error)
{
  g_autoptr (GBytes) groups = NULL;
  g_autoptr (GBytes) episodes = NULL;
  g_autofree char *group = NULL;
  if (self->omdb || match->tmdb_id < 0)
    return empty_episodes ();
  groups = fetch (self, bro_tmdb_requests_episode_groups (match->tmdb_id, self->key, self->language), cancel, error);
  if (!groups)
    return NULL;
  group = bro_tmdb_parse_absolute_group (groups);
  if (!group)
    return empty_episodes ();
  episodes = fetch (self, bro_tmdb_requests_episode_group (group, self->key, self->language), cancel, error);
  return episodes ? bro_tmdb_parse_absolute_episodes (episodes) : NULL;
}

gboolean
bro_metadata_client_poster (BroMetadataClient *self, const char *url, const char *destination, BroCancellationToken *cancel, BroBroError **error)
{
  g_autoptr (BroHttpRequestSpec) request = bro_http_request_spec_new ("GET", url);
  g_autoptr (BroHttpResponse) response = bro_http_client_send (self->http, request, cancel, error);
  if (!response)
    return FALSE;
  if (response->status != 200)
    {
      set_http_error (self, response->status, error);
      return FALSE;
    }
  return bro_file_system_write_atomically (self->fs, destination, response->body, 0644, error);
}

static void
bro_metadata_client_finalize (GObject *object)
{
  BroMetadataClient *self = BRO_METADATA_CLIENT (object);
  g_clear_object (&self->http);
  g_clear_object (&self->cache);
  g_clear_object (&self->clock);
  g_clear_object (&self->fs);
  g_free (self->key);
  g_free (self->language);
  G_OBJECT_CLASS (bro_metadata_client_parent_class)->finalize (object);
}

static void
bro_metadata_client_class_init (BroMetadataClientClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_metadata_client_finalize;
}

static void
bro_metadata_client_init (BroMetadataClient *self)
{
}

BroMetadataClient *
bro_metadata_client_new (BroHttpClient *http, BroLookupCacheRepository *cache, BroClock *clock, BroFileSystem *fs, const char *provider,
                         const char *key, const char *language)
{
  BroMetadataClient *self = g_object_new (BRO_TYPE_METADATA_CLIENT, NULL);
  self->http = g_object_ref (http);
  self->cache = cache ? g_object_ref (cache) : NULL;
  self->clock = g_object_ref (clock);
  self->fs = g_object_ref (fs);
  self->omdb = g_strcmp0 (provider, "omdb") == 0;
  self->key = g_strdup (key);
  self->language = g_strdup (language && *language ? language : "en-US");
  return self;
}
