/* test-metadata.inc: MetadataClient, included by test-adapters.c: shared/fixtures/adapters/metadata-client.cases.json. */

/* An HttpClient that answers by URL: the first rule whose match is part of the URL. */
#define TEST_TYPE_ROUTED_HTTP (test_routed_http_get_type ())
G_DECLARE_FINAL_TYPE (TestRoutedHttp, test_routed_http, TEST, ROUTED_HTTP, GObject)

struct _TestRoutedHttp {
  GObject parent_instance;
  BroJsonValue *rules;
  GPtrArray *urls;
};

static void test_routed_http_iface_init (BroHttpClientInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestRoutedHttp, test_routed_http, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_HTTP_CLIENT, test_routed_http_iface_init))

static BroHttpResponse *
routed_send (BroHttpClient *c, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  TestRoutedHttp *self = TEST_ROUTED_HTTP (c);
  BroJsonValue *rule = NULL;
  BroHttpResponse *r;
  const char *file, *reason;
  g_ptr_array_add (self->urls, g_strdup (request->url));
  for (guint i = 0; !rule && i < bro_json_value_length (self->rules); i++)
    if (strstr (request->url, bro_json_value_get_string (bro_json_value_member (bro_json_value_at (self->rules, i), "match"), "\x01")))
      rule = bro_json_value_at (self->rules, i);
  if (!rule)
    {
      bro_bro_error_set (error, "test.noAnswer", NULL);
      return NULL;
    }
  if ((reason = bro_json_value_get_string (bro_json_value_member (rule, "reason"), NULL)))
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
      bro_bro_error_set (error, "http.failed", params);
      return NULL;
    }
  r = bro_http_response_new ();
  r->status = (int) bro_json_value_get_integer (bro_json_value_member (rule, "status"), 200);
  g_clear_pointer (&r->body, g_bytes_unref);
  if ((file = bro_json_value_get_string (bro_json_value_member (rule, "file"), NULL)))
    {
      gsize n = 0;
      char *text = bro_test_fixture_text (file, &n);
      r->body = g_bytes_new_take (text, n);
    }
  else
    {
      const char *text = bro_json_value_get_string (bro_json_value_member (rule, "text"), "");
      r->body = g_bytes_new (text, strlen (text));
    }
  return r;
}

static void
test_routed_http_finalize (GObject *o)
{
  TestRoutedHttp *self = TEST_ROUTED_HTTP (o);
  g_clear_pointer (&self->rules, bro_json_value_unref);
  g_ptr_array_unref (self->urls);
  G_OBJECT_CLASS (test_routed_http_parent_class)->finalize (o);
}

static void test_routed_http_class_init (TestRoutedHttpClass *k) { G_OBJECT_CLASS (k)->finalize = test_routed_http_finalize; }
static void test_routed_http_init (TestRoutedHttp *self) { self->urls = g_ptr_array_new_with_free_func (g_free); }
static void test_routed_http_iface_init (BroHttpClientInterface *iface) { iface->send = routed_send; }

static BroCandidate *
match_for (const char *choice)
{
  g_autoptr (BroOnlineId) id = bro_online_id_parse (choice);
  BroCandidate *c = g_new0 (BroCandidate, 1);
  c->title = g_strdup ("");
  c->year = -1;
  c->tmdb_id = -1;
  c->overview = g_strdup ("");
  c->poster = g_strdup ("");
  c->has_kind = TRUE;
  c->kind = BRO_MEDIA_KIND_TV;
  if (id && id->kind == BRO_ONLINE_ID_TMDB)
    {
      c->tmdb_id = id->tmdb_id;
      c->provider = g_strdup ("TMDb");
      if (id->has_media_kind)
        c->kind = id->media_kind;
    }
  else
    {
      c->imdb_id = g_strdup (choice);
      c->provider = g_strdup ("OMDb");
    }
  return c;
}

static int metadata_case_number;

static gboolean
metadata_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autofree char *name = g_strdup_printf ("metadata%d", metadata_case_number++);
  g_autoptr (BroSqliteStore) store = new_store (name);
  g_autoptr (TestFixedClock) clock = g_object_new (TEST_TYPE_FIXED_CLOCK, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (TestRoutedHttp) http = g_object_new (TEST_TYPE_ROUTED_HTTP, NULL);
  g_autoptr (BroMetadataClient) client = NULL;
  g_autoptr (GPtrArray) results = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  g_autoptr (GPtrArray) episodes = g_ptr_array_new_with_free_func ((GDestroyNotify) g_hash_table_unref);
  g_autoptr (BroBroError) error = NULL;
  BroJsonValue *calls = bro_json_value_member (given, "calls");
  BroJsonValue *want = bro_json_value_member (expect, "error");
  BroJsonValue *v;
  http->rules = bro_json_value_ref (bro_json_value_member (given, "answers"));
  client = bro_metadata_client_new (BRO_HTTP_CLIENT (http), bro_store_lookup_cache (BRO_STORE (store)), BRO_CLOCK (clock), BRO_FILE_SYSTEM (fs),
                                    bro_json_value_get_string (bro_json_value_member (given, "provider"), ""), "0123456789abcdef0123456789abcdef", "en-US");
  for (guint i = 0; !error && i < bro_json_value_length (calls); i++)
    {
      BroJsonValue *c = bro_json_value_at (calls, i);
      const char *what = bro_json_value_get_string (bro_json_value_at (c, 0), "");
      const char *a1 = bro_json_value_get_string (bro_json_value_at (c, 1), "");
      BroMediaKind kind = BRO_MEDIA_KIND_MOVIE;
      if (bro_json_value_length (c) > 2 && bro_json_value_at (c, 2)->kind == BRO_JSON_VALUE_STRING)
        bro_media_kind_from_wire (bro_json_value_get_string (bro_json_value_at (c, 2), ""), &kind);
      if (g_str_equal (what, "search"))
        {
          BroJsonValue *y = bro_json_value_at (c, 3);
          GPtrArray *r = bro_metadata_client_search (client, a1, kind, y->kind == BRO_JSON_VALUE_NULL ? -1 : (int) y->integer, NULL, &error);
          if (r)
            g_ptr_array_add (results, r);
        }
      else if (g_str_equal (what, "lookup"))
        {
          g_autoptr (BroOnlineId) oid = bro_online_id_parse (a1);
          BroCandidate *found = bro_metadata_client_lookup (client, oid, kind, NULL, &error);
          GPtrArray *r = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_candidate_free);
          if (found)
            g_ptr_array_add (r, found);
          g_ptr_array_add (results, r);
        }
      else if (g_str_equal (what, "season") || g_str_equal (what, "absoluteEpisodes"))
        {
          g_autoptr (BroCandidate) m = match_for (a1);
          GHashTable *e = g_str_equal (what, "season")
                            ? bro_metadata_client_season (client, m, (int) bro_json_value_get_integer (bro_json_value_at (c, 2), 0), NULL, &error)
                            : bro_metadata_client_absolute_episodes (client, m, NULL, &error);
          if (e)
            g_ptr_array_add (episodes, e);
        }
      else if (g_str_equal (what, "poster"))
        {
          g_autofree char *dest = g_build_filename (store_dir, bro_json_value_get_string (bro_json_value_at (c, 2), ""), NULL);
          bro_metadata_client_poster (client, a1, dest, NULL, &error);
        }
      else
        return FALSE;
    }
  if (want)
    {
      BroJsonValue *params = bro_json_value_member (want, "params");
      bro_test_same_string (failures, id, "code", bro_json_value_get_string (bro_json_value_member (want, "code"), ""), error ? error->code : NULL);
      for (guint i = 0; error && params && i < bro_json_value_length (params); i++)
        bro_test_same_json (failures, id, params->keys->pdata[i], bro_json_value_at (params, i), bro_json_value_member (error->params, params->keys->pdata[i]));
    }
  else if (error)
    bro_test_fail (failures, id, "failed: %s", error->code);
  if ((v = bro_json_value_member (expect, "results")))
    for (guint i = 0; i < bro_json_value_length (v) && i < results->len; i++)
      {
        BroJsonValue *list = bro_json_value_at (v, i);
        GPtrArray *got = results->pdata[i];
        for (guint k = 0; k < bro_json_value_length (list); k++)
          {
            BroJsonValue *w = bro_json_value_at (list, k);
            BroCandidate *c = k < got->len ? got->pdata[k] : NULL;
            BroJsonValue *f;
            if (!c)
              {
                bro_test_fail (failures, id, "result %u.%u missing", i, k);
                continue;
              }
            if ((f = bro_json_value_member (w, "title")))
              bro_test_same_string (failures, id, "title", bro_json_value_get_string (f, ""), c->title);
            if ((f = bro_json_value_member (w, "year")) && f->integer != c->year)
              bro_test_fail (failures, id, "year %d", c->year);
            if ((f = bro_json_value_member (w, "tmdbId")) && f->integer != c->tmdb_id)
              bro_test_fail (failures, id, "tmdbId %d", c->tmdb_id);
            if ((f = bro_json_value_member (w, "overviewStarts")) && !g_str_has_prefix (c->overview, bro_json_value_get_string (f, "")))
              bro_test_fail (failures, id, "overview %s", c->overview);
          }
      }
  if ((v = bro_json_value_member (expect, "episodes")))
    for (guint i = 0; i < bro_json_value_length (v) && i < episodes->len; i++)
      {
        BroJsonValue *e = bro_json_value_at (v, i);
        GHashTable *got = episodes->pdata[i];
        for (guint k = 0; k < bro_json_value_length (e); k++)
          {
            const char *key = e->keys->pdata[k];
            if (g_str_equal (key, "count"))
              {
                if ((gint64) g_hash_table_size (got) != bro_json_value_get_integer (bro_json_value_at (e, k), -1))
                  bro_test_fail (failures, id, "count %u", g_hash_table_size (got));
              }
            else
              {
                BroEpisodeDetails *d = g_hash_table_lookup (got, GINT_TO_POINTER (atoi (key)));
                bro_test_same_string (failures, id, key, bro_json_value_get_string (bro_json_value_at (e, k), ""), d ? d->title : NULL);
              }
          }
      }
  if ((v = bro_json_value_member (expect, "requests")) && (gint64) http->urls->len != bro_json_value_get_integer (v, -1))
    bro_test_fail (failures, id, "%u requests", http->urls->len);
  if ((v = bro_json_value_member (expect, "lastRequestLacks")) && http->urls->len
      && strstr (http->urls->pdata[http->urls->len - 1], bro_json_value_get_string (v, "")))
    bro_test_fail (failures, id, "the last request has %s", bro_json_value_get_string (v, ""));
  if ((v = bro_json_value_member (expect, "cacheKeyLacks")))
    {
      sqlite3 *db = raw_db (name);
      g_autoptr (GPtrArray) keys = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
      dump_rows (db, "SELECT request_key FROM lookup_cache", keys);
      if (!keys->len || strstr (((char **) keys->pdata[0])[0], bro_json_value_get_string (v, "")))
        bro_test_fail (failures, id, "cache key");
      sqlite3_close (db);
    }
  if ((v = bro_json_value_member (expect, "file")))
    for (guint i = 0; i < bro_json_value_length (v); i++)
      {
        g_autofree char *path = g_build_filename (store_dir, v->keys->pdata[i], NULL);
        g_autofree char *text = NULL;
        g_file_get_contents (path, &text, NULL, NULL);
        bro_test_same_string (failures, id, "file", bro_json_value_get_string (bro_json_value_at (v, i), ""), text);
      }
  return TRUE;
}

static void
test_metadata_client (void)
{
  bro_test_run_cases ("adapters/metadata-client.cases.json", metadata_case);
}
