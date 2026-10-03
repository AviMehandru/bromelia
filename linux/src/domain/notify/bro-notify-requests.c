/* bro-notify-requests.c */
#include "bro-notify-requests.h"

#include "bro-json-value.h"

#include <string.h>

static BroDelivery *
post (const char *url, GBytes *body)
{
  BroDelivery *d = g_new0 (BroDelivery, 1);
  d->kind = BRO_DELIVERY_HTTP;
  d->request = bro_http_request_spec_new ("POST", url);
  d->request->body = body;
  return d;
}

static BroDelivery *
apprise (const char *url)
{
  BroDelivery *d = g_new0 (BroDelivery, 1);
  d->kind = BRO_DELIVERY_APPRISE;
  d->url = g_strdup (url);
  return d;
}

/* A JSON POST; takes ownership of @members (an object). */
static BroDelivery *
post_json (const char *url, BroJsonValue *members)
{
  BroDelivery *d = post (url, bro_json_value_encode_canonical (members));
  bro_json_value_unref (members);
  bro_http_request_spec_add_header (d->request, "Content-Type", "application/json");
  return d;
}

static BroDelivery *
post_text (const char *url, const char *title, gboolean success, const char *body)
{
  BroDelivery *d = post (url, g_bytes_new (body, strlen (body)));
  bro_http_request_spec_add_header (d->request, "Title", title);
  bro_http_request_spec_add_header (d->request, "Tags", success ? "white_check_mark" : "warning");
  return d;
}

static void
put (BroJsonValue *o, const char *key, const char *value)
{
  bro_json_value_set (o, key, bro_json_value_new_string (value));
}

/* The host of an http(s) URL's authority (without user info or port), lower case, and its path. */
static void
host_and_path (const char *rest, char **host, char **path)
{
  gsize end = strcspn (rest, "/?#");
  g_autofree char *authority = g_strndup (rest, end);
  *path = rest[end] == '/' ? g_strndup (rest + end, strcspn (rest + end, "?#")) : g_strdup ("");
  const char *h = strrchr (authority, '@');
  h = h ? h + 1 : authority;
  gsize n;
  if (h[0] == '[') {
    const char *close = strchr (h, ']');
    n = close ? (gsize) (close - h) + 1 : 0;
  } else {
    n = strcspn (h, ":");
  }
  g_autofree char *raw = g_strndup (h, n);
  *host = g_ascii_strdown (raw, -1);
}

/* Apprise URLs sent without Python: Telegram with one chat (tgram://bot_token/chat_id), Pushover
 * (pover://user_key@app_token[/device]) and Gotify (gotify://host[:port][/path]/app_token, gotifys:// for HTTPS).
 * NULL for the other forms, which go to the apprise command. */
static BroDelivery *
direct (const char *scheme, const char *rest, const char *title, const char *body, gboolean success)
{
  g_auto (GStrv) parts = g_strsplit (rest, "/", -1);
  guint count = g_strv_length (parts);
  if (count > 0 && parts[count - 1][0] == '\0')
    count--;
  if (strcmp (scheme, "tgram") == 0) {
    if (count != 2 || !strchr (parts[0], ':') || !parts[1][0])
      return NULL;
    g_autofree char *url = g_strdup_printf ("https://api.telegram.org/bot%s/sendMessage", parts[0]);
    g_autofree char *text = g_strdup_printf ("%s\n%s", title, body);
    BroJsonValue *o = bro_json_value_new_object ();
    put (o, "chat_id", parts[1]);
    put (o, "text", text);
    return post_json (url, o);
  }
  if (strcmp (scheme, "pover") == 0) {
    const char *at = count >= 1 && count <= 2 ? strchr (parts[0], '@') : NULL;
    if (!at || at == parts[0] || at[1] == '\0')
      return NULL;
    g_autofree char *user = g_strndup (parts[0], at - parts[0]);
    BroJsonValue *o = bro_json_value_new_object ();
    put (o, "token", at + 1);
    put (o, "user", user);
    put (o, "title", title);
    put (o, "message", body);
    if (count == 2 && parts[1][0])
      put (o, "device", parts[1]);
    return post_json ("https://api.pushover.net/1/messages.json", o);
  }
  if (count < 2 || !parts[0][0] || !parts[count - 1][0])
    return NULL;
  g_autofree char *key = g_strdup (parts[count - 1]);
  g_free (parts[count - 1]);
  parts[count - 1] = NULL;
  g_autofree char *hostpath = g_strjoinv ("/", parts);
  g_autofree char *url = g_strdup_printf ("%s://%s/message", strcmp (scheme, "gotifys") == 0 ? "https" : "http", hostpath);
  BroJsonValue *o = bro_json_value_new_object ();
  put (o, "title", title);
  put (o, "message", body);
  bro_json_value_set (o, "priority", bro_json_value_new_integer (success ? 5 : 8));
  BroDelivery *d = post_json (url, o);
  bro_http_request_spec_add_header (d->request, "X-Gotify-Key", key);
  return d;
}

BroDelivery *
bro_notify_requests_build (const char *url, const char *title, const char *body, BroStatusWord status)
{
  g_autofree char *raw = g_strstrip (g_strdup (url));
  const char *sep = strstr (raw, "://");
  if (!sep || sep == raw)
    return NULL;
  g_autofree char *scheme_raw = g_strndup (raw, sep - raw);
  g_autofree char *scheme = g_ascii_strdown (scheme_raw, -1);
  const char *rest = sep + 3;
  gboolean success = status == BRO_STATUS_WORD_SUCCESS;
  if (strcmp (scheme, "http") == 0 || strcmp (scheme, "https") == 0) {
    g_autofree char *host = NULL, *path = NULL;
    host_and_path (rest, &host, &path);
    if (!*host)
      return NULL;
    if ((g_str_has_suffix (host, "discord.com") || g_str_has_suffix (host, "discordapp.com")) && strstr (path, "/api/webhooks/")) {
      g_autofree char *content = g_strdup_printf ("**%s**\n%s", title, body);
      BroJsonValue *o = bro_json_value_new_object ();
      put (o, "content", content);
      return post_json (raw, o);
    }
    if (strcmp (host, "hooks.slack.com") == 0) {
      g_autofree char *text = g_strdup_printf ("*%s*\n%s", title, body);
      BroJsonValue *o = bro_json_value_new_object ();
      put (o, "text", text);
      return post_json (raw, o);
    }
    if (strcmp (host, "ntfy.sh") == 0)
      return post_text (raw, title, success, body);
    BroJsonValue *o = bro_json_value_new_object ();
    put (o, "app", "Bromelia");
    put (o, "body", body);
    put (o, "status", bro_status_word_to_wire (status));
    put (o, "title", title);
    return post_json (raw, o);
  }
  if (strcmp (scheme, "ntfy") == 0 || strcmp (scheme, "ntfys") == 0) {
    /* Apprise's form: ntfy://topic (ntfy.sh), ntfy://host/topic, ntfys://host/topic. */
    g_autofree char *topic = g_strdup (rest);
    gsize n = strlen (topic);
    while (n > 0 && topic[n - 1] == '/')
      topic[--n] = '\0';
    g_autofree char *target = strchr (topic, '/') ? g_strdup_printf ("%s://%s", strcmp (scheme, "ntfys") == 0 ? "https" : "http", topic)
                                                  : g_strdup_printf ("https://ntfy.sh/%s", topic);
    return post_text (target, title, success, body);
  }
  if (strcmp (scheme, "tgram") == 0 || strcmp (scheme, "pover") == 0 || strcmp (scheme, "gotify") == 0 || strcmp (scheme, "gotifys") == 0) {
    BroDelivery *d = direct (scheme, rest, title, body, success);
    return d ? d : apprise (raw);
  }
  return apprise (raw);
}

GPtrArray *
bro_notify_requests_targets_for (GPtrArray *targets, BroStatusWord status)
{
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; i < targets->len; i++) {
    BroNotifyTarget *t = targets->pdata[i];
    if (t->enabled && !(t->only_problems && status == BRO_STATUS_WORD_SUCCESS))
      g_ptr_array_add (out, t);
  }
  return out;
}
