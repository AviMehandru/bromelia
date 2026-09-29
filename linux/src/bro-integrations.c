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
  g_free (m);
}

gboolean
bro_metadata_request (const char *name, BroMediaKind kind, const BroMetadataConfig *config, char **url, char **bearer)
{
  g_autofree char *key = g_strstrip (g_strdup (config->api_key ? config->api_key : ""));
  g_autofree char *q = NULL;
  *url = NULL;
  *bearer = NULL;
  if (!*key || !name || !*name)
    return FALSE;
  q = g_uri_escape_string (name, NULL, FALSE);
  switch (config->provider)
    {
    case BRO_METADATA_TMDB:
      {
        g_autofree char *lang = g_uri_escape_string (config->language && *config->language ? config->language : "en-US", NULL, FALSE);
        g_autofree char *base = g_strdup_printf ("https://api.themoviedb.org/3/search/%s?query=%s&language=%s",
                                                 kind == BRO_KIND_TV ? "tv" : "movie", q, lang);
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
    case BRO_METADATA_OMDB:
      {
        g_autofree char *k = g_uri_escape_string (key, NULL, FALSE);
        *url = g_strdup_printf ("https://www.omdbapi.com/?apikey=%s&t=%s&type=%s", k, q, kind == BRO_KIND_TV ? "series" : "movie");
        return TRUE;
      }
    default:
      return FALSE;
    }
}

static char *
normalize (const char *s)
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

static const char *
obj_str (JsonObject *o, const char *k)
{
  JsonNode *n = o && json_object_has_member (o, k) ? json_object_get_member (o, k) : NULL;
  return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING ? json_node_get_string (n) : NULL;
}

static int
year_of (const char *s)
{
  if (!s || strlen (s) < 4 || !g_ascii_isdigit (s[0]) || !g_ascii_isdigit (s[1]) || !g_ascii_isdigit (s[2]) || !g_ascii_isdigit (s[3]))
    return 0;
  return (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

static const char *
tmdb_title (JsonObject *r)
{
  const char *t = obj_str (r, "title");
  return t ? t : obj_str (r, "name");
}

BroMediaMatch *
bro_metadata_parse (const char *json, BroMetadataProvider provider, const char *name)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonObject *o;
  BroMediaMatch *m;
  if (!json || !json_parser_load_from_data (parser, json, -1, NULL))
    return NULL;
  root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;
  o = json_node_get_object (root);
  if (provider == BRO_METADATA_TMDB)
    {
      JsonNode *rn = json_object_has_member (o, "results") ? json_object_get_member (o, "results") : NULL;
      JsonArray *results = rn && JSON_NODE_HOLDS_ARRAY (rn) ? json_node_get_array (rn) : NULL;
      g_autofree char *want = normalize (name);
      JsonObject *pick = NULL, *first = NULL;
      const char *title, *date;
      for (guint i = 0; results && i < json_array_get_length (results); i++)
        {
          JsonNode *n = json_array_get_element (results, i);
          JsonObject *r;
          if (!JSON_NODE_HOLDS_OBJECT (n))
            continue;
          r = json_node_get_object (n);
          if (!first)
            first = r;
          {
            g_autofree char *have = normalize (tmdb_title (r));
            if (g_str_equal (have, want))
              {
                pick = r;
                break;
              }
          }
        }
      if (!pick)
        pick = first;
      if (!pick || !(title = tmdb_title (pick)) || !*title)
        return NULL;
      date = obj_str (pick, "release_date");
      if (!date)
        date = obj_str (pick, "first_air_date");
      m = g_new0 (BroMediaMatch, 1);
      m->title = g_strdup (title);
      m->year = year_of (date);
      if (json_object_has_member (pick, "id") && JSON_NODE_HOLDS_VALUE (json_object_get_member (pick, "id")))
        m->tmdb_id = (int) json_object_get_int_member (pick, "id");
      m->provider = "TMDb";
      return m;
    }
  if (provider == BRO_METADATA_OMDB && g_strcmp0 (obj_str (o, "Response"), "True") == 0 && obj_str (o, "Title"))
    {
      m = g_new0 (BroMediaMatch, 1);
      m->title = g_strdup (obj_str (o, "Title"));
      m->year = year_of (obj_str (o, "Year"));
      m->imdb_id = g_strdup (obj_str (o, "imdbID"));
      m->provider = "OMDb";
      return m;
    }
  return NULL;
}

BroMediaMatch *
bro_metadata_lookup (const char *name, BroMediaKind kind, const BroMetadataConfig *config, GError **error)
{
  g_autofree char *url = NULL, *bearer = NULL, *auth = NULL;
  g_autoptr (GString) body = g_string_new (NULL);
  const char *headers[] = { "Accept: application/json", NULL, NULL };
  int code = 0;
  if (!bro_metadata_request (name, kind, config, &url, &bearer))
    return NULL;
  if (bearer)
    headers[1] = auth = g_strdup_printf ("Authorization: Bearer %s", bearer);
  if (!bro_http_request ("GET", url, headers, NULL, 0, 20, &code, body, error))
    return NULL;
  if (code < 200 || code >= 300)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s answered HTTP %d",
                   config->provider == BRO_METADATA_TMDB ? "TMDb" : "OMDb", code);
      return NULL;
    }
  return bro_metadata_parse (body->str, config->provider, name);
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
