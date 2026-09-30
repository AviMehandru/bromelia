/* bro-integrations.c — online services, media server names, audio CDs and data discs. */
#include "bro-integrations.h"
#include "bro-logic.h"
#include "bro-makemkv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* ---- HTTP ---- */

/* A string for a curl config file: quoted, with \ " and line breaks escaped. */
static void
append_curl_string (GString *out, const char *key, const char *s)
{
  g_string_append_printf (out, "%s = \"", key);
  for (const char *p = s ? s : ""; *p; p++)
    switch (*p)
      {
      case '\\': g_string_append (out, "\\\\"); break;
      case '"': g_string_append (out, "\\\""); break;
      case '\n': g_string_append (out, "\\n"); break;
      case '\r': g_string_append (out, "\\r"); break;
      case '\t': g_string_append (out, "\\t"); break;
      default: g_string_append_c (out, *p);
      }
  g_string_append (out, "\"\n");
}

static void
remove_temp_dir (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      g_autofree char *p = g_build_filename (dir, name, NULL);
      g_unlink (p);
    }
  g_rmdir (dir);
}

static void
append_line (const char *line, gpointer data)
{
  g_string_append ((GString *) data, line);
}

gboolean
bro_http_request (const char *method, const char *url, const char *const *headers, const char *body, gsize body_len,
                  int timeout_seconds, int *status, GString *response, GError **error)
{
  g_autofree char *curl = g_find_program_in_path ("curl");
  g_autofree char *dir = NULL, *config_path = NULL, *out_path = NULL;
  g_autoptr (GString) cfg = g_string_new (NULL);
  g_autoptr (GString) code = g_string_new (NULL);
  int timeout = timeout_seconds > 0 ? timeout_seconds : 30;
  g_autofree char *timeout_text = g_strdup_printf ("%d", timeout);
  int exit_status = -1;
  gboolean ok = FALSE;

  if (status)
    *status = 0;
  if (!curl)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "curl is not installed");
      return FALSE;
    }
  /* g_dir_make_tmp makes a 0700 folder, so keys in the config file stay private (and out of `ps`). */
  if (!(dir = g_dir_make_tmp ("bromelia-http-XXXXXX", error)))
    return FALSE;
  config_path = g_build_filename (dir, "request.conf", NULL);
  out_path = g_build_filename (dir, "response", NULL);
  append_curl_string (cfg, "url", url);
  append_curl_string (cfg, "request", method ? method : "GET");
  for (int i = 0; headers && headers[i]; i++)
    append_curl_string (cfg, "header", headers[i]);
  if (body)
    {
      g_autofree char *body_path = g_build_filename (dir, "body", NULL);
      g_autofree char *arg = g_strconcat ("@", body_path, NULL);
      if (!g_file_set_contents (body_path, body, (gssize) body_len, error))
        goto out;
      append_curl_string (cfg, "data-binary", arg);
    }
  append_curl_string (cfg, "output", out_path);
  if (!g_file_set_contents (config_path, cfg->str, (gssize) cfg->len, error))
    goto out;
  {
    const char *argv[] = { curl, "-sS", "--max-time", timeout_text, "-K", config_path, "-w", "%{http_code}", NULL };
    if (!bro_process_run (argv, NULL, NULL, timeout + 10, NULL, append_line, code, &exit_status, NULL, NULL, error))
      goto out;
    if (exit_status != 0)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "curl failed (exit status %d)", exit_status);
        goto out;
      }
  }
  if (status)
    *status = atoi (code->str);
  if (response)
    {
      g_autofree char *text = NULL;
      gsize len = 0;
      if (g_file_get_contents (out_path, &text, &len, NULL))
        g_string_append_len (response, text, (gssize) len);
    }
  ok = TRUE;
out:
  remove_temp_dir (dir);
  return ok;
}

/* ---- notifications ---- */

void
bro_delivery_free (BroDelivery *d)
{
  if (!d)
    return;
  g_free (d->url);
  if (d->headers)
    g_ptr_array_unref (d->headers);
  g_free (d->body);
  g_free (d->apprise_url);
  g_free (d);
}

static char *
json_object_text (const char *const *pairs)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  json_builder_begin_object (b);
  for (int i = 0; pairs[i]; i += 2)
    {
      json_builder_set_member_name (b, pairs[i]);
      json_builder_add_string_value (b, pairs[i + 1]);
    }
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  return bro_json_to_string (root, FALSE);
}

static BroDelivery *
delivery_new (const char *url)
{
  BroDelivery *d = g_new0 (BroDelivery, 1);
  d->url = g_strdup (url);
  d->headers = g_ptr_array_new_with_free_func (g_free);
  return d;
}

static BroDelivery *
json_delivery (const char *url, char *body)
{
  BroDelivery *d = delivery_new (url);
  g_ptr_array_add (d->headers, g_strdup ("Content-Type: application/json"));
  d->body = body;
  return d;
}

static BroDelivery *
ntfy_delivery (const char *url, const char *title, const char *body, const char *status)
{
  BroDelivery *d = delivery_new (url);
  g_autofree char *one_line = g_strdelimit (g_strdup (title), "\r\n", ' ');
  g_ptr_array_add (d->headers, g_strdup_printf ("Title: %s", one_line));
  g_ptr_array_add (d->headers, g_strdup_printf ("Tags: %s", g_strcmp0 (status, "success") == 0 ? "white_check_mark" : "warning"));
  d->body = g_strdup (body);
  return d;
}

BroDelivery *
bro_delivery_for (const char *target, const char *title, const char *body, const char *status)
{
  g_autofree char *raw = g_strstrip (g_strdup (target ? target : ""));
  const char *sep = strstr (raw, "://");
  g_autofree char *scheme = NULL;
  if (!sep || sep == raw)
    return NULL;
  scheme = g_ascii_strdown (raw, sep - raw);
  if (g_str_equal (scheme, "http") || g_str_equal (scheme, "https"))
    {
      g_autoptr (GUri) uri = g_uri_parse (raw, G_URI_FLAGS_NONE, NULL);
      g_autofree char *host = NULL;
      const char *path;
      if (!uri || !g_uri_get_host (uri) || !*g_uri_get_host (uri))
        return NULL;
      host = g_ascii_strdown (g_uri_get_host (uri), -1);
      path = g_uri_get_path (uri);
      if ((g_str_has_suffix (host, "discord.com") || g_str_has_suffix (host, "discordapp.com")) && strstr (path, "/api/webhooks/"))
        {
          g_autofree char *text = g_strdup_printf ("**%s**\n%s", title, body);
          const char *pairs[] = { "content", text, NULL };
          return json_delivery (raw, json_object_text (pairs));
        }
      if (g_str_equal (host, "hooks.slack.com"))
        {
          g_autofree char *text = g_strdup_printf ("*%s*\n%s", title, body);
          const char *pairs[] = { "text", text, NULL };
          return json_delivery (raw, json_object_text (pairs));
        }
      if (g_str_equal (host, "ntfy.sh"))
        return ntfy_delivery (raw, title, body, status);
      {
        const char *pairs[] = { "app", "Bromelia", "body", body, "status", status, "title", title, NULL };
        return json_delivery (raw, json_object_text (pairs));
      }
    }
  if (g_str_equal (scheme, "ntfy") || g_str_equal (scheme, "ntfys"))
    {
      /* Apprise's form: ntfy://topic (ntfy.sh), ntfy://host/topic, ntfys://host/topic. */
      g_autofree char *rest = g_strdup (sep + 3);
      g_autofree char *url = NULL;
      size_t n = strlen (rest);
      while (n && rest[n - 1] == '/')
        rest[--n] = '\0';
      if (!*rest)
        return NULL;
      url = strchr (rest, '/') ? g_strdup_printf ("%s://%s", g_str_equal (scheme, "ntfys") ? "https" : "http", rest)
                               : g_strdup_printf ("https://ntfy.sh/%s", rest);
      return ntfy_delivery (url, title, body, status);
    }
  {
    BroDelivery *d = g_new0 (BroDelivery, 1);
    d->apprise_url = g_strdup (raw);
    return d;
  }
}

static void
say (BroTextFunc log, gpointer data, const char *fmt, ...) G_GNUC_PRINTF (3, 4);

static void
say (BroTextFunc log, gpointer data, const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;
  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  if (log)
    log (text, data);
}

static char *
url_host (const char *url)
{
  g_autoptr (GUri) u = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  return g_strdup (u && g_uri_get_host (u) ? g_uri_get_host (u) : url);
}

void
bro_notifications_send (GPtrArray *targets, const char *title, const char *body, const char *status, BroTextFunc log, gpointer data)
{
  for (guint i = 0; targets && i < targets->len; i++)
    {
      BroNotificationTarget *t = targets->pdata[i];
      g_autoptr (BroDelivery) d = NULL;
      if (!t->enabled || (t->only_problems && g_strcmp0 (status, "success") == 0))
        continue;
      d = bro_delivery_for (t->url, title, body, status);
      if (!d)
        {
          say (log, data, "Notification: can't use “%s”", t->url);
          continue;
        }
      if (d->apprise_url)
        {
          g_autofree char *apprise = bro_find_tool ("", "apprise");
          const char *argv[] = { apprise, "-t", title, "-b", body, d->apprise_url, NULL };
          int st = -1;
          g_autoptr (GError) err = NULL;
          if (!apprise)
            say (log, data, "Notification: “%s” needs the apprise command (pip install apprise)", d->apprise_url);
          else if (!bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, &st, NULL, NULL, &err))
            say (log, data, "Notification with apprise failed: %s", err ? err->message : "unknown error");
          else if (st != 0)
            say (log, data, "Notification with apprise failed (exit status %d)", st);
          continue;
        }
      {
        g_autoptr (GError) err = NULL;
        g_autofree char *host = url_host (d->url);
        int code = 0;
        g_ptr_array_add (d->headers, NULL);
        if (!bro_http_request ("POST", d->url, (const char *const *) d->headers->pdata, d->body, strlen (d->body), 20, &code, NULL, &err))
          say (log, data, "Notification to %s failed: %s", host, err ? err->message : "unknown error");
        else if (code < 200 || code >= 300)
          say (log, data, "Notification to %s failed: HTTP %d", host, code);
      }
    }
}

/* ---- online lookup ---- */

void
bro_media_match_free (BroMediaMatch *m)
{
  if (!m)
    return;
  g_free (m->title);
  g_free (m->imdb_id);
  g_free (m->overview);
  g_free (m->poster);
  g_free (m);
}

static BroMediaMatch *
media_match_new (const char *title, const char *provider, int kind)
{
  BroMediaMatch *m = g_new0 (BroMediaMatch, 1);
  m->title = g_strdup (title);
  m->provider = provider;
  m->kind = kind;
  m->overview = g_strdup ("");
  m->poster = g_strdup ("");
  return m;
}

BroMediaMatch *
bro_media_match_copy (const BroMediaMatch *m)
{
  BroMediaMatch *c;
  if (!m)
    return NULL;
  c = media_match_new (m->title, m->provider, m->kind);
  c->year = m->year;
  c->tmdb_id = m->tmdb_id;
  c->imdb_id = g_strdup (m->imdb_id);
  g_free (c->overview);
  c->overview = g_strdup (m->overview ? m->overview : "");
  g_free (c->poster);
  c->poster = g_strdup (m->poster ? m->poster : "");
  return c;
}

char *
bro_media_match_label (const BroMediaMatch *m)
{
  return m->year ? g_strdup_printf ("%s (%d)", m->title, m->year) : g_strdup (m->title);
}

char *
bro_media_match_choice (const BroMediaMatch *m)
{
  if (m->tmdb_id)
    return g_strdup_printf ("%s/%d", m->kind == BRO_KIND_TV ? "tv" : "movie", m->tmdb_id);
  return g_strdup (m->imdb_id ? m->imdb_id : "");
}

void
bro_episode_details_free (BroEpisodeDetails *e)
{
  if (!e)
    return;
  g_free (e->title);
  g_free (e->overview);
  g_free (e->aired);
  g_free (e);
}

char *
bro_normalize_title (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s ? s : ""; *p; p = g_utf8_next_char (p))
    {
      gunichar ch = g_utf8_get_char (p);
      if (g_unichar_isalnum (ch))
        g_string_append_unichar (out, g_unichar_tolower (ch));
    }
  return g_string_free (out, FALSE);
}

char *
bro_metadata_split_year (const char *name, int *year)
{
  g_autofree char *t = g_strstrip (g_strdup (name ? name : ""));
  g_autoptr (GRegex) re = g_regex_new ("^(.*\\S)\\s*\\((\\d{4})\\)$", 0, 0, NULL);
  g_autoptr (GMatchInfo) mi = NULL;
  *year = 0;
  if (g_regex_match (re, t, 0, &mi))
    {
      g_autofree char *y = g_match_info_fetch (mi, 2);
      int n = atoi (y);
      if (n >= 1870 && n <= 2100)
        {
          *year = n;
          return g_match_info_fetch (mi, 1);
        }
    }
  return g_steal_pointer (&t);
}

static int
kind_of_word (const char *w)
{
  return !w || !*w ? -1 : g_str_equal (w, "tv") ? BRO_KIND_TV : BRO_KIND_MOVIE;
}

gboolean
bro_online_id_parse (const char *text, int *tmdb, int *kind, char **imdb)
{
  g_autofree char *t = g_ascii_strdown (text ? text : "", -1);
  g_autoptr (GRegex) tt = g_regex_new ("tt\\d{5,10}", 0, 0, NULL);
  g_autoptr (GRegex) site = g_regex_new ("themoviedb\\.org/(movie|tv)/(\\d+)", 0, 0, NULL);
  g_autoptr (GRegex) plain = g_regex_new ("^(?:tmdb:)?(?:(movie|tv)/)?(\\d{1,9})$", 0, 0, NULL);
  g_autoptr (GMatchInfo) mi = NULL;
  *tmdb = 0;
  *kind = -1;
  *imdb = NULL;
  g_strstrip (t);
  if (g_regex_match (tt, t, 0, &mi))
    {
      *imdb = g_match_info_fetch (mi, 0);
      return TRUE;
    }
  g_clear_pointer (&mi, g_match_info_free);
  if (!g_regex_match (site, t, 0, &mi))
    {
      g_clear_pointer (&mi, g_match_info_free);
      if (!g_regex_match (plain, t, 0, &mi))
        return FALSE;
    }
    {
      g_autofree char *k = g_match_info_fetch (mi, 1);
      g_autofree char *n = g_match_info_fetch (mi, 2);
      int id = atoi (n);
      if (id <= 0)
        return FALSE;
      *tmdb = id;
      *kind = kind_of_word (k);
      return TRUE;
    }
}

char *
bro_online_id_label (int tmdb, int kind, const char *imdb)
{
  if (imdb)
    return g_strdup (imdb);
  if (kind < 0)
    return g_strdup_printf ("TMDb %d", tmdb);
  return g_strdup_printf ("TMDb %s/%d", kind == BRO_KIND_TV ? "tv" : "movie", tmdb);
}

/* A TMDb request: an API key (v3) goes into the query, a read access token into a Bearer header. */
static gboolean
tmdb_request (const char *path, const char *query, const BroMetadataConfig *config, char **url, char **bearer)
{
  g_autofree char *key = g_strstrip (g_strdup (config->api_key ? config->api_key : ""));
  g_autofree char *lang = g_uri_escape_string (config->language && *config->language ? config->language : "en-US", NULL, FALSE);
  g_autofree char *base = NULL;
  *url = NULL;
  *bearer = NULL;
  if (!*key)
    return FALSE;
  base = g_strdup_printf ("https://api.themoviedb.org/3/%s?%s%slanguage=%s", path, query ? query : "", query && *query ? "&" : "", lang);
  /* A v3 API key is 32 characters; a read access token is a long JWT sent as a bearer token. */
  if (strlen (key) <= 40)
    {
      g_autofree char *k = g_uri_escape_string (key, NULL, FALSE);
      *url = g_strdup_printf ("%s&api_key=%s", base, k);
    }
  else
    {
      *url = g_steal_pointer (&base);
      *bearer = g_strdup (key);
    }
  return TRUE;
}

static gboolean
omdb_request (const char *query, const BroMetadataConfig *config, char **url, char **bearer)
{
  g_autofree char *key = g_strstrip (g_strdup (config->api_key ? config->api_key : ""));
  g_autofree char *k = NULL;
  *url = NULL;
  *bearer = NULL;
  if (!*key)
    return FALSE;
  k = g_uri_escape_string (key, NULL, FALSE);
  *url = g_strdup_printf ("https://www.omdbapi.com/?apikey=%s&%s", k, query);
  return TRUE;
}

gboolean
bro_metadata_request (const char *name, BroMediaKind kind, int year, const BroMetadataConfig *config, char **url, char **bearer)
{
  g_autofree char *q = NULL, *query = NULL;
  *url = NULL;
  *bearer = NULL;
  if (!name || !*name)
    return FALSE;
  q = g_uri_escape_string (name, NULL, FALSE);
  switch (config->provider)
    {
    case BRO_METADATA_TMDB:
      {
        g_autofree char *path = g_strdup_printf ("search/%s", kind == BRO_KIND_TV ? "tv" : "movie");
        query = year ? g_strdup_printf ("query=%s&%s=%d", q, kind == BRO_KIND_TV ? "first_air_date_year" : "year", year)
                     : g_strdup_printf ("query=%s", q);
        return tmdb_request (path, query, config, url, bearer);
      }
    case BRO_METADATA_OMDB:
      query = year ? g_strdup_printf ("s=%s&type=%s&y=%d", q, kind == BRO_KIND_TV ? "series" : "movie", year)
                   : g_strdup_printf ("s=%s&type=%s", q, kind == BRO_KIND_TV ? "series" : "movie");
      return omdb_request (query, config, url, bearer);
    default:
      return FALSE;
    }
}

gboolean
bro_metadata_id_request (int tmdb, int id_kind, const char *imdb, BroMediaKind kind, const BroMetadataConfig *config, char **url,
                         char **bearer)
{
  *url = NULL;
  *bearer = NULL;
  if (config->provider == BRO_METADATA_TMDB && tmdb > 0)
    {
      g_autofree char *path = g_strdup_printf ("%s/%d", (id_kind >= 0 ? id_kind : (int) kind) == BRO_KIND_TV ? "tv" : "movie", tmdb);
      return tmdb_request (path, "", config, url, bearer);
    }
  if (config->provider == BRO_METADATA_TMDB && imdb)
    {
      g_autofree char *path = g_strdup_printf ("find/%s", imdb);
      return tmdb_request (path, "external_source=imdb_id", config, url, bearer);
    }
  if (config->provider == BRO_METADATA_OMDB && imdb)
    {
      g_autofree char *i = g_uri_escape_string (imdb, NULL, FALSE);
      g_autofree char *query = g_strdup_printf ("i=%s&plot=full", i);
      return omdb_request (query, config, url, bearer);
    }
  return FALSE;
}

gboolean
bro_metadata_season_request (const BroMediaMatch *match, int season, const BroMetadataConfig *config, char **url, char **bearer)
{
  *url = NULL;
  *bearer = NULL;
  if (config->provider == BRO_METADATA_TMDB && match->tmdb_id)
    {
      g_autofree char *path = g_strdup_printf ("tv/%d/season/%d", match->tmdb_id, season);
      return tmdb_request (path, "", config, url, bearer);
    }
  if (config->provider == BRO_METADATA_OMDB && match->imdb_id)
    {
      g_autofree char *i = g_uri_escape_string (match->imdb_id, NULL, FALSE);
      g_autofree char *query = g_strdup_printf ("i=%s&Season=%d", i, season);
      return omdb_request (query, config, url, bearer);
    }
  return FALSE;
}

static const char *
obj_str (JsonObject *o, const char *k)
{
  JsonNode *n = o && json_object_has_member (o, k) ? json_object_get_member (o, k) : NULL;
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING ? json_node_get_string (n) : NULL;
}

/* A text field, "" for missing values and OMDb's "N/A". */
static const char *
obj_text (JsonObject *o, const char *k)
{
  const char *s = obj_str (o, k);
  return s && !g_str_equal (s, "N/A") ? s : "";
}

static int
obj_int (JsonObject *o, const char *k)
{
  JsonNode *n = o && json_object_has_member (o, k) ? json_object_get_member (o, k) : NULL;
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_INT64 ? (int) json_node_get_int (n) : 0;
}

static JsonArray *
obj_arr (JsonObject *o, const char *k)
{
  JsonNode *n = o && json_object_has_member (o, k) ? json_object_get_member (o, k) : NULL;
  return n && JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

static int
year_of (const char *s)
{
  if (!s || strlen (s) < 4 || !g_ascii_isdigit (s[0]) || !g_ascii_isdigit (s[1]) || !g_ascii_isdigit (s[2]) || !g_ascii_isdigit (s[3]))
    return 0;
  return (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

static BroMediaMatch *
tmdb_match (JsonObject *r, int kind)
{
  const char *title = obj_str (r, "title");
  const char *date = obj_str (r, "release_date");
  const char *imdb = obj_text (r, "imdb_id"), *poster = obj_str (r, "poster_path");
  BroMediaMatch *m;
  if (!title)
    title = obj_str (r, "name");
  if (!title || !*title)
    return NULL;
  if (!date)
    date = obj_str (r, "first_air_date");
  m = media_match_new (title, "TMDb", kind);
  m->year = year_of (date);
  m->tmdb_id = obj_int (r, "id");
  m->imdb_id = *imdb ? g_strdup (imdb) : NULL;
  g_free (m->overview);
  m->overview = g_strdup (obj_text (r, "overview"));
  if (poster)
    {
      g_free (m->poster);
      m->poster = g_strconcat ("https://image.tmdb.org/t/p/original", poster, NULL);
    }
  return m;
}

static BroMediaMatch *
omdb_match (JsonObject *r, int kind)
{
  const char *title = obj_str (r, "Title"), *type = obj_str (r, "Type");
  BroMediaMatch *m;
  if (!title || !*title)
    return NULL;
  m = media_match_new (title, "OMDb", g_strcmp0 (type, "series") == 0 ? BRO_KIND_TV : g_strcmp0 (type, "movie") == 0 ? BRO_KIND_MOVIE : kind);
  m->year = year_of (obj_str (r, "Year"));
  m->imdb_id = g_strdup (obj_str (r, "imdbID"));
  g_free (m->overview);
  m->overview = g_strdup (obj_text (r, "Plot"));
  g_free (m->poster);
  m->poster = g_strdup (obj_text (r, "Poster"));
  return m;
}

static JsonObject *
parse_object (JsonParser *parser, const char *json)
{
  JsonNode *root;
  if (!json || !json_parser_load_from_data (parser, json, -1, NULL))
    return NULL;
  root = json_parser_get_root (parser);
  return root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
}

typedef struct {
  BroMediaMatch *m;
  int score, order;
} Ranked;

static int
cmp_ranked (gconstpointer a, gconstpointer b)
{
  const Ranked *x = a, *y = b;
  return x->score != y->score ? y->score - x->score : x->order - y->order;
}

GPtrArray *
bro_metadata_candidates (const char *json, BroMetadataProvider provider, const char *name, int year, BroMediaKind kind)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *o = parse_object (parser, json);
  g_autoptr (GPtrArray) found = g_ptr_array_new ();
  g_autoptr (GArray) ranked = g_array_new (FALSE, FALSE, sizeof (Ranked));
  g_autofree char *want = bro_normalize_title (name);
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_media_match_free);
  JsonArray *arr;
  if (!o)
    return out;
  if (provider == BRO_METADATA_TMDB && (arr = obj_arr (o, "results")))
    {
      for (guint i = 0; i < json_array_get_length (arr); i++)
        if (JSON_NODE_HOLDS_OBJECT (json_array_get_element (arr, i)))
          {
            BroMediaMatch *m = tmdb_match (json_array_get_object_element (arr, i), kind);
            if (m)
              g_ptr_array_add (found, m);
          }
    }
  else if (provider == BRO_METADATA_OMDB && g_strcmp0 (obj_str (o, "Response"), "True") == 0)
    {
      if ((arr = obj_arr (o, "Search")))
        {
          for (guint i = 0; i < json_array_get_length (arr); i++)
            if (JSON_NODE_HOLDS_OBJECT (json_array_get_element (arr, i)))
              {
                BroMediaMatch *m = omdb_match (json_array_get_object_element (arr, i), kind);
                if (m)
                  g_ptr_array_add (found, m);
              }
        }
      else
        {
          BroMediaMatch *m = omdb_match (o, kind);
          if (m)
            g_ptr_array_add (found, m);
        }
    }
  for (guint i = 0; i < found->len; i++)
    {
      BroMediaMatch *m = found->pdata[i];
      g_autofree char *have = bro_normalize_title (m->title);
      Ranked r = { m, g_str_equal (have, want) ? 4 : 0, (int) i };
      if (year && m->year)
        r.score += m->year == year ? 2 : ABS (m->year - year) == 1 ? 1 : 0;
      g_array_append_val (ranked, r);
    }
  g_array_sort (ranked, cmp_ranked);
  for (guint i = 0; i < ranked->len; i++)
    g_ptr_array_add (out, g_array_index (ranked, Ranked, i).m);
  return out;
}

BroMediaMatch *
bro_metadata_parse (const char *json, BroMetadataProvider provider, const char *name)
{
  g_autoptr (GPtrArray) list = bro_metadata_candidates (json, provider, name, 0, BRO_KIND_MOVIE);
  return list->len ? g_ptr_array_steal_index (list, 0) : NULL;
}

BroMediaMatch *
bro_metadata_details (const char *json, BroMetadataProvider provider, BroMediaKind kind)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *o = parse_object (parser, json);
  if (!o)
    return NULL;
  if (provider == BRO_METADATA_TMDB)
    {
      if (json_object_has_member (o, "movie_results") || json_object_has_member (o, "tv_results"))
        {
          /* The kind the id belongs to, whatever the disc was taken for. */
          JsonArray *movies = obj_arr (o, "movie_results"), *shows = obj_arr (o, "tv_results");
          JsonObject *movie = movies && json_array_get_length (movies) && JSON_NODE_HOLDS_OBJECT (json_array_get_element (movies, 0))
                                ? json_array_get_object_element (movies, 0) : NULL;
          JsonObject *show = shows && json_array_get_length (shows) && JSON_NODE_HOLDS_OBJECT (json_array_get_element (shows, 0))
                               ? json_array_get_object_element (shows, 0) : NULL;
          if (kind == BRO_KIND_TV)
            return show ? tmdb_match (show, BRO_KIND_TV) : movie ? tmdb_match (movie, BRO_KIND_MOVIE) : NULL;
          return movie ? tmdb_match (movie, BRO_KIND_MOVIE) : show ? tmdb_match (show, BRO_KIND_TV) : NULL;
        }
      if (!json_object_has_member (o, "id"))
        return NULL;
      return tmdb_match (o, json_object_has_member (o, "name") && !json_object_has_member (o, "title") ? BRO_KIND_TV : BRO_KIND_MOVIE);
    }
  if (provider == BRO_METADATA_OMDB && g_strcmp0 (obj_str (o, "Response"), "True") == 0)
    return omdb_match (o, kind);
  return NULL;
}

GHashTable *
bro_metadata_parse_season (const char *json, BroMetadataProvider provider)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *o = parse_object (parser, json);
  GHashTable *out = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_episode_details_free);
  JsonArray *arr = o ? obj_arr (o, provider == BRO_METADATA_TMDB ? "episodes" : "Episodes") : NULL;
  for (guint i = 0; arr && provider != BRO_METADATA_NONE && i < json_array_get_length (arr); i++)
    {
      JsonObject *e;
      const char *title;
      int n;
      BroEpisodeDetails *d;
      if (!JSON_NODE_HOLDS_OBJECT (json_array_get_element (arr, i)))
        continue;
      e = json_array_get_object_element (arr, i);
      if (provider == BRO_METADATA_TMDB)
        {
          n = obj_int (e, "episode_number");
          title = obj_str (e, "name");
        }
      else
        {
          const char *num = obj_str (e, "Episode");
          n = num && *num && strspn (num, "0123456789") == strlen (num) ? atoi (num) : 0;
          title = obj_str (e, "Title");
        }
      if (n <= 0 || !title || !*title)
        continue;
      d = g_new0 (BroEpisodeDetails, 1);
      d->title = g_strdup (title);
      d->overview = g_strdup (provider == BRO_METADATA_TMDB ? obj_text (e, "overview") : "");
      d->aired = g_strdup (obj_text (e, provider == BRO_METADATA_TMDB ? "air_date" : "Released"));
      g_hash_table_replace (out, GINT_TO_POINTER (n), d);
    }
  return out;
}

static const char *
provider_name (const BroMetadataConfig *config)
{
  return config->provider == BRO_METADATA_TMDB ? "TMDb" : "OMDb";
}

/* GET url; the body, or NULL with error (HTTP errors too). */
static char *
fetch (const char *url, const char *bearer, const BroMetadataConfig *config, GError **error)
{
  g_autoptr (GString) body = g_string_new (NULL);
  g_autofree char *auth = bearer ? g_strdup_printf ("Authorization: Bearer %s", bearer) : NULL;
  const char *headers[] = { "Accept: application/json", auth, NULL };
  int code = 0;
  if (!bro_http_request ("GET", url, headers, NULL, 0, 20, &code, body, error))
    return NULL;
  if (code < 200 || code >= 300)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s answered HTTP %d", provider_name (config), code);
      return NULL;
    }
  return g_string_free (g_steal_pointer (&body), FALSE);
}

GPtrArray *
bro_metadata_search (const char *name, BroMediaKind kind, int year, const BroMetadataConfig *config, GError **error)
{
  g_autofree char *url = NULL, *bearer = NULL, *json = NULL;
  GPtrArray *list = NULL;
  if (!bro_metadata_request (name, kind, year, config, &url, &bearer))
    return g_ptr_array_new_with_free_func ((GDestroyNotify) bro_media_match_free);
  if (!(json = fetch (url, bearer, config, error)))
    return NULL;
  list = bro_metadata_candidates (json, config->provider, name, year, kind);
  if (!list->len && year)
    {
      g_clear_pointer (&url, g_free);
      g_clear_pointer (&bearer, g_free);
      g_clear_pointer (&json, g_free);
      bro_metadata_request (name, kind, 0, config, &url, &bearer);
      if (!(json = fetch (url, bearer, config, error)))
        {
          g_ptr_array_unref (list);
          return NULL;
        }
      g_ptr_array_unref (list);
      list = bro_metadata_candidates (json, config->provider, name, year, kind);
    }
  /* OMDb's search results have no plot: read the best one again. */
  if (config->provider == BRO_METADATA_OMDB && list->len && ((BroMediaMatch *) list->pdata[0])->imdb_id)
    {
      g_autoptr (BroMediaMatch) full = bro_metadata_lookup_id (0, -1, ((BroMediaMatch *) list->pdata[0])->imdb_id, kind, config, NULL);
      if (full)
        {
          bro_media_match_free (list->pdata[0]);
          list->pdata[0] = g_steal_pointer (&full);
        }
    }
  return list;
}

BroMediaMatch *
bro_metadata_lookup_id (int tmdb, int id_kind, const char *imdb, BroMediaKind kind, const BroMetadataConfig *config, GError **error)
{
  g_autofree char *url = NULL, *bearer = NULL, *json = NULL;
  if (!bro_metadata_id_request (tmdb, id_kind, imdb, kind, config, &url, &bearer))
    {
      g_autofree char *label = bro_online_id_label (tmdb, id_kind, imdb);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s can't look up %s%s", provider_name (config), label,
                   config->provider == BRO_METADATA_OMDB ? " (it takes IMDb ids, tt…)" : "");
      return NULL;
    }
  if (!(json = fetch (url, bearer, config, error)))
    return NULL;
  return bro_metadata_details (json, config->provider, kind);
}

GHashTable *
bro_metadata_episodes (const BroMediaMatch *match, int season, const BroMetadataConfig *config, GError **error)
{
  g_autofree char *url = NULL, *bearer = NULL, *json = NULL;
  if (!bro_metadata_season_request (match, season, config, &url, &bearer))
    return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_episode_details_free);
  if (!(json = fetch (url, bearer, config, error)))
    return NULL;
  return bro_metadata_parse_season (json, config->provider);
}

/* ---- media server metadata ---- */

gboolean
bro_is_metadata_file (const char *name)
{
  g_autofree char *lower = g_ascii_strdown (name, -1);
  return g_str_has_suffix (lower, ".nfo") || g_str_equal (name, BRO_POSTER_NAME);
}

static void
xml_element (GString *out, const char *name, const char *close, const char *value)
{
  if (!value || !*value)
    return;
  g_string_append_printf (out, "  <%s>", name);
  for (const char *p = value; *p; p++)
    switch (*p)
      {
      case '&': g_string_append (out, "&amp;"); break;
      case '<': g_string_append (out, "&lt;"); break;
      case '>': g_string_append (out, "&gt;"); break;
      default: g_string_append_c (out, *p);
      }
  g_string_append_printf (out, "</%s>\n", close ? close : name);
}

static GString *
nfo_start (const char *root)
{
  GString *out = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  g_string_append_printf (out, "<%s>\n", root);
  return out;
}

char *
bro_nfo (const BroMediaMatch *m, BroMediaKind kind)
{
  const char *root = kind == BRO_KIND_TV ? "tvshow" : "movie";
  GString *out = nfo_start (root);
  g_autofree char *year = m->year ? g_strdup_printf ("%d", m->year) : NULL;
  g_autofree char *tmdb = m->tmdb_id ? g_strdup_printf ("%d", m->tmdb_id) : NULL;
  gboolean first = TRUE;
  xml_element (out, "title", NULL, m->title);
  xml_element (out, "year", NULL, year);
  xml_element (out, "plot", NULL, m->overview);
  if (tmdb)
    {
      xml_element (out, "uniqueid type=\"tmdb\" default=\"true\"", "uniqueid", tmdb);
      first = FALSE;
    }
  if (m->imdb_id && *m->imdb_id)
    xml_element (out, first ? "uniqueid type=\"imdb\" default=\"true\"" : "uniqueid type=\"imdb\"", "uniqueid", m->imdb_id);
  g_string_append_printf (out, "</%s>\n", root);
  return g_string_free (out, FALSE);
}

char *
bro_episode_nfo (const char *show, int season, int episode, const BroEpisodeDetails *d)
{
  GString *out = nfo_start ("episodedetails");
  g_autofree char *s = g_strdup_printf ("%d", season), *e = g_strdup_printf ("%d", episode);
  xml_element (out, "title", NULL, d->title);
  xml_element (out, "showtitle", NULL, show);
  xml_element (out, "season", NULL, s);
  xml_element (out, "episode", NULL, e);
  xml_element (out, "plot", NULL, d->overview);
  xml_element (out, "aired", NULL, d->aired);
  g_string_append (out, "</episodedetails>\n");
  return g_string_free (out, FALSE);
}

gboolean
bro_http_download (const char *url, const char *dest, GError **error)
{
  g_autoptr (GString) body = g_string_new (NULL);
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFileOutputStream) out = NULL;
  int code = 0;
  if (!bro_http_request ("GET", url, NULL, NULL, 0, 60, &code, body, error))
    return FALSE;
  if (code != 200 || !body->len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, code != 200 ? "HTTP %d" : "empty answer", code);
      return FALSE;
    }
  file = g_file_new_for_path (dest);
  if (!(out = g_file_create (file, G_FILE_CREATE_NONE, NULL, error)))
    return FALSE;
  if (!g_output_stream_write_all (G_OUTPUT_STREAM (out), body->str, body->len, NULL, NULL, error)
      || !g_output_stream_close (G_OUTPUT_STREAM (out), NULL, error))
    {
      g_file_delete (file, NULL, NULL);
      return FALSE;
    }
  return TRUE;
}

/* ---- media server layout ---- */

const char *
bro_media_server_file_template (GHashTable *values, gboolean is_main_feature)
{
  const char *e = g_hash_table_lookup (values, "episodeNumber");
  if (e && *e)
    return BRO_MEDIA_EPISODE_TEMPLATE;
  if (is_main_feature && g_strcmp0 (g_hash_table_lookup (values, "kind"), "movie") == 0)
    return BRO_MEDIA_MAIN_TEMPLATE;
  return BRO_MEDIA_OTHER_TEMPLATE;
}

/* ---- discs without a DVD / Blu-ray structure ---- */

static gboolean
has_video_structure (const char *root)
{
  g_autoptr (GDir) d = root ? g_dir_open (root, 0, NULL) : NULL;
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    if (g_ascii_strcasecmp (name, "BDMV") == 0 || g_ascii_strcasecmp (name, "VIDEO_TS") == 0 || g_ascii_strcasecmp (name, "HVDVD_TS") == 0)
      return TRUE;
  return FALSE;
}

static void
collect_line (const char *line, gpointer data)
{
  g_string_append_printf ((GString *) data, "%s\n", line);
}

static int
udev_int (GHashTable *props, const char *key)
{
  const char *v = g_hash_table_lookup (props, key);
  return v ? atoi (v) : 0;
}

BroDiscContent
bro_disc_content_probe (const char *device)
{
  g_autofree char *udevadm = bro_find_tool ("", "udevadm");
  g_autoptr (GString) out = g_string_new (NULL);
  g_autoptr (GHashTable) props = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autofree char *mp = NULL;
  g_auto (GStrv) lines = NULL;
  int st = -1;
  if (!device || !*device)
    return BRO_CONTENT_UNKNOWN;
  /* A DVD / Blu-ray structure on the mounted disc means a video disc, whatever MakeMKV's flags said. */
  mp = bro_disc_mount_point (device);
  if (mp && has_video_structure (mp))
    return BRO_CONTENT_VIDEO;
  if (udevadm)
    {
      const char *argv[] = { udevadm, "info", "--query=property", "--name", device, NULL };
      if (!bro_process_run (argv, NULL, NULL, 20, NULL, collect_line, out, &st, NULL, NULL, NULL) || st != 0)
        return BRO_CONTENT_UNKNOWN;
    }
  lines = g_strsplit (out->str, "\n", -1);
  for (char **l = lines; *l; l++)
    {
      char *eq = strchr (*l, '=');
      if (eq)
        g_hash_table_replace (props, g_strndup (*l, eq - *l), g_strdup (eq + 1));
    }
  if (g_strcmp0 (g_hash_table_lookup (props, "ID_CDROM_MEDIA"), "1") != 0)
    return BRO_CONTENT_UNKNOWN;
  /* Audio tracks make an audio CD (enhanced CDs have a data session too). */
  if (udev_int (props, "ID_CDROM_MEDIA_TRACK_COUNT_AUDIO") > 0)
    return BRO_CONTENT_AUDIO;
  if (udev_int (props, "ID_CDROM_MEDIA_TRACK_COUNT_DATA") > 0)
    return BRO_CONTENT_DATA;
  return BRO_CONTENT_UNKNOWN;
}

gboolean
bro_disc_mode_for (int drive_flags, BroContentProbe probe, const char *device, const BroDriveConfig *drive, BroRipMode *mode)
{
  BroDiscContent content;
  *mode = drive->rip.mode;
  if (drive_flags < 0 || (drive_flags & (BRO_DISC_DVD | BRO_DISC_BLURAY | BRO_DISC_HDDVD)))
    return TRUE;
  content = probe ? probe (device) : BRO_CONTENT_UNKNOWN;
  switch (content)
    {
    case BRO_CONTENT_AUDIO:
      *mode = BRO_MODE_AUDIO_CD;
      return drive->other.rip_audio_cds;
    case BRO_CONTENT_DATA:
      *mode = BRO_MODE_DATA_IMAGE;
      return drive->other.image_data_discs;
    default:
      return TRUE;
    }
}

GPtrArray *
bro_audio_command (const BroOtherDiscs *other, const char *device)
{
  g_autofree char *custom = g_strstrip (g_strdup (other->audio_command ? other->audio_command : ""));
  GPtrArray *argv;
  if (*custom)
    {
      g_autoptr (GPtrArray) parts = bro_split_arguments (custom);
      g_autoptr (GHashTable) values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      char *exe;
      if (!parts->len)
        return NULL;
      g_hash_table_insert (values, g_strdup ("device"), g_strdup (device));
      exe = g_path_is_absolute (parts->pdata[0]) ? (g_file_test (parts->pdata[0], G_FILE_TEST_IS_EXECUTABLE) ? g_strdup (parts->pdata[0]) : NULL)
                                                  : bro_find_tool ("", parts->pdata[0]);
      if (!exe)
        return NULL;
      argv = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (argv, exe);
      for (guint i = 1; i < parts->len; i++)
        g_ptr_array_add (argv, bro_template_render (parts->pdata[i], values, FALSE));
      return argv;
    }
  {
    char *cyanrip = bro_find_tool ("", "cyanrip");
    char *abcde = cyanrip ? NULL : bro_find_tool ("", "abcde");
    if (!cyanrip && !abcde)
      return NULL;
    argv = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (argv, cyanrip ? cyanrip : abcde);
    bro_ptr_array_add_all (argv, "-d", device, "-o", "flac", NULL);
    if (abcde)
      g_ptr_array_add (argv, g_strdup ("-N"));
    return argv;
  }
}

gboolean
bro_copy_disc (const char *device, const char *dest, BroCopyProgress progress, gpointer data, GCancellable *cancellable, GError **error)
{
  int in, out;
  gint64 done = 0, total = -1;
  gsize size = 2048 * 512;
  g_autofree char *buf = g_malloc (size);
  gboolean ok = TRUE;

  in = g_open (device, O_RDONLY, 0);
  if (in < 0)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Could not open %s: %s", device, g_strerror (e));
      return FALSE;
    }
  out = g_open (dest, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (out < 0)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Could not create %s: %s", dest, g_strerror (e));
      close (in);
      return FALSE;
    }
  {
    off_t end = lseek (in, 0, SEEK_END);
    if (end > 0)
      total = end;
    lseek (in, 0, SEEK_SET);
  }
  for (;;)
    {
      ssize_t n = read (in, buf, size);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        {
          int e = errno;
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Read error at byte %" G_GINT64_FORMAT ": %s", done, g_strerror (e));
          ok = FALSE;
          break;
        }
      if (n == 0)
        break;
      for (ssize_t w = 0; w < n;)
        {
          ssize_t k = write (out, buf + w, n - w);
          if (k < 0 && errno == EINTR)
            continue;
          if (k < 0)
            {
              int e = errno;
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Could not write %s: %s", dest, g_strerror (e));
              ok = FALSE;
              break;
            }
          w += k;
        }
      if (!ok)
        break;
      done += n;
      if (g_cancellable_set_error_if_cancelled (cancellable, error) || (progress && !progress (done, total, data)))
        {
          if (error && !*error)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          ok = FALSE;
          break;
        }
    }
  close (in);
  if (ok && fsync (out) != 0)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Could not write %s: %s", dest, g_strerror (e));
      ok = FALSE;
    }
  if (close (out) != 0 && ok)
    {
      int e = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (e), "Could not write %s: %s", dest, g_strerror (e));
      ok = FALSE;
    }
  if (!ok)
    g_unlink (dest);
  return ok;
}

/* ---- drive control ---- */

gboolean
bro_close_tray (const char *device)
{
  g_autofree char *eject = bro_find_tool ("", "eject");
  int st = -1;
  if (!eject || !device || !*device)
    return FALSE;
  {
    const char *argv[] = { eject, "-t", device, NULL };
    return bro_process_run (argv, NULL, NULL, 30, NULL, NULL, NULL, &st, NULL, NULL, NULL) && st == 0;
  }
}

char *
bro_disc_mount_point (const char *device)
{
  g_autofree char *text = NULL;
  g_autofree char *real = NULL;
  g_auto (GStrv) lines = NULL;
  if (!device || !*device || !g_file_get_contents ("/proc/self/mounts", &text, NULL, NULL))
    return NULL;
  real = g_canonicalize_filename (device, NULL);
  {
    /* /dev/cdrom and friends are links to /dev/sr0. */
    char resolved[4096];
    ssize_t n = readlink (device, resolved, sizeof resolved - 1);
    if (n > 0)
      {
        g_autofree char *dir = g_path_get_dirname (device);
        resolved[n] = '\0';
        g_free (real);
        real = g_canonicalize_filename (resolved, dir);
      }
  }
  lines = g_strsplit (text, "\n", -1);
  for (char **l = lines; *l; l++)
    {
      g_auto (GStrv) f = g_strsplit (*l, " ", 3);
      if (f[0] && f[1] && (g_str_equal (f[0], device) || g_str_equal (f[0], real)))
        {
          GString *mp = g_string_new (f[1]);
          g_string_replace (mp, "\\040", " ", 0);
          return g_string_free (mp, FALSE);
        }
    }
  return NULL;
}

gboolean
bro_disc_is_mounted (const char *device)
{
  g_autofree char *mp = bro_disc_mount_point (device);
  return mp != NULL;
}
