/* bro-web.c — the web page and its JSON API. */
#include "bro-web.h"

#include <string.h>

#define MAX_HEAD 65536

void
bro_http_request_free (BroHttpRequest *r)
{
  if (!r)
    return;
  g_free (r->method);
  g_free (r->path);
  g_hash_table_unref (r->query);
  g_hash_table_unref (r->headers);
  g_free (r);
}

BroHttpRequest *
bro_http_request_parse (const char *data, gsize len)
{
  const char *end = g_strstr_len (data, (gssize) len, "\r\n\r\n");
  g_autofree char *head = NULL;
  g_auto (GStrv) lines = NULL;
  g_auto (GStrv) first = NULL;
  BroHttpRequest *r;
  char *target, *q;
  guint n = 0;

  if (!end)
    return NULL;
  head = g_strndup (data, end - data);
  lines = g_strsplit (head, "\r\n", -1);
  first = g_strsplit_set (lines[0], " ", -1);
  {
    /* Collapse repeated spaces. */
    GPtrArray *parts = g_ptr_array_new ();
    for (char **p = first; *p; p++)
      if (**p)
        g_ptr_array_add (parts, *p);
    n = parts->len;
    if (n < 2)
      {
        g_ptr_array_unref (parts);
        return NULL;
      }
    r = g_new0 (BroHttpRequest, 1);
    r->method = g_ascii_strup (parts->pdata[0], -1);
    target = g_strdup (parts->pdata[1]);
    g_ptr_array_unref (parts);
  }
  r->query = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r->headers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  q = strchr (target, '?');
  if (q)
    {
      g_auto (GStrv) pairs = g_strsplit (q + 1, "&", -1);
      *q = '\0';
      for (char **p = pairs; *p; p++)
        {
          char *eq = strchr (*p, '=');
          g_autofree char *k = eq ? g_strndup (*p, eq - *p) : g_strdup (*p);
          g_autofree char *v = g_strdup (eq ? eq + 1 : "");
          g_strdelimit (k, "+", ' ');
          g_strdelimit (v, "+", ' ');
          {
            char *dk = g_uri_unescape_string (k, NULL);
            char *dv = g_uri_unescape_string (v, NULL);
            if (dk && *dk)
              g_hash_table_replace (r->query, dk, dv ? dv : g_strdup (""));
            else
              {
                g_free (dk);
                g_free (dv);
              }
          }
        }
    }
  r->path = target;
  for (int i = 1; lines[i]; i++)
    {
      char *colon = strchr (lines[i], ':');
      if (!colon)
        continue;
      {
        g_autofree char *name = g_strstrip (g_strndup (lines[i], colon - lines[i]));
        char *value = g_strstrip (g_strdup (colon + 1));
        g_hash_table_replace (r->headers, g_ascii_strdown (name, -1), value);
      }
    }
  return r;
}

int
bro_web_access (const BroHttpRequest *r, const BroWebUIConfig *config, const char **why)
{
  g_autofree char *token = g_strstrip (g_strdup (config->token ? config->token : ""));
  *why = NULL;
  if (*token)
    {
      const char *auth = g_hash_table_lookup (r->headers, "authorization");
      const char *bearer = auth && g_str_has_prefix (auth, "Bearer ") ? auth + 7 : "";
      if (g_strcmp0 (bearer, token) != 0 && g_strcmp0 (g_hash_table_lookup (r->query, "token"), token) != 0)
        {
          *why = "A token is required";
          return 401;
        }
    }
  else
    {
      const char *h = g_hash_table_lookup (r->headers, "host");
      g_autofree char *host = g_ascii_strdown (h ? h : "", -1);
      g_autofree char *name = NULL;
      if (host[0] == '[')
        {
          char *close = strchr (host, ']');
          name = close ? g_strndup (host, close - host + 1) : g_strdup (host);
        }
      else
        {
          char *colon = strchr (host, ':');
          name = colon ? g_strndup (host, colon - host) : g_strdup (host);
        }
      if (!g_str_equal (name, "localhost") && !g_str_equal (name, "127.0.0.1") && !g_str_equal (name, "[::1]"))
        {
          *why = "Set a token to use Bromelia from another computer";
          return 403;
        }
    }
  if (g_str_equal (r->method, "POST") && g_strcmp0 (g_hash_table_lookup (r->headers, "x-bromelia"), "1") != 0)
    {
      *why = "Missing X-Bromelia header";
      return 403;
    }
  return 200;
}

static gboolean
is_loopback (const char *address)
{
  return g_str_equal (address, "127.0.0.1") || g_str_equal (address, "localhost") || g_str_equal (address, "::1");
}

char *
bro_web_start_problem (const BroWebUIConfig *config)
{
  g_autofree char *address = g_strstrip (g_strdup (config->address ? config->address : ""));
  g_autofree char *token = g_strstrip (g_strdup (config->token ? config->token : ""));
  if (!is_loopback (address) && !*token)
    return g_strdup ("The web page is reachable from the network only with a token. Set a token or use 127.0.0.1.");
  if (config->port < 1 || config->port > 65535)
    return g_strdup_printf ("Port %d is not valid", config->port);
  return NULL;
}

GBytes *
bro_web_page (void)
{
  return g_resources_lookup_data ("/app/bromelia/Bromelia/bromelia-web.html", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
}

/* ---- server ---- */

struct _BroWebServer {
  GSocketService *service;
  BroWebUIConfig running; /* copy of the config it runs with (valid while service != NULL) */
  char *last_error;
  BroWebStatusFunc status;
  BroWebActionFunc action;
  gpointer user_data;
};

typedef struct {
  BroWebServer *server;
  GSocketConnection *conn;
  GByteArray *buf;
  char chunk[4096];
  GBytes *out;
} Conn;

static void
conn_free (Conn *c)
{
  g_io_stream_close (G_IO_STREAM (c->conn), NULL, NULL);
  g_object_unref (c->conn);
  g_byte_array_unref (c->buf);
  if (c->out)
    g_bytes_unref (c->out);
  g_free (c);
}

static void
config_clear (BroWebUIConfig *c)
{
  g_clear_pointer (&c->address, g_free);
  g_clear_pointer (&c->token, g_free);
}

BroWebServer *
bro_web_server_new (BroWebStatusFunc status, BroWebActionFunc action, gpointer user_data)
{
  BroWebServer *s = g_new0 (BroWebServer, 1);
  s->status = status;
  s->action = action;
  s->user_data = user_data;
  return s;
}

void
bro_web_server_stop (BroWebServer *s)
{
  if (s->service)
    {
      g_socket_service_stop (s->service);
      g_socket_listener_close (G_SOCKET_LISTENER (s->service));
      g_clear_object (&s->service);
    }
  config_clear (&s->running);
}

void
bro_web_server_free (BroWebServer *s)
{
  if (!s)
    return;
  bro_web_server_stop (s);
  g_free (s->last_error);
  g_free (s);
}

gboolean
bro_web_server_running (BroWebServer *s)
{
  return s->service != NULL;
}

const char *
bro_web_server_last_error (BroWebServer *s)
{
  return s->last_error;
}

static const char *
reason (int status)
{
  switch (status)
    {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    default: return "Error";
    }
}

static GBytes *
text_bytes (const char *s)
{
  return g_bytes_new (s, strlen (s));
}

int
bro_web_server_handle (BroWebServer *s, const BroHttpRequest *r, const char **content_type, GBytes **body)
{
  const char *why = NULL;
  int code;
  g_auto (GStrv) raw = NULL;
  g_autoptr (GPtrArray) parts = g_ptr_array_new_with_free_func (g_free);

  *content_type = "text/plain; charset=utf-8";
  if (!s->service && !s->running.address)
    {
      *body = text_bytes ("");
      return 404;
    }
  code = bro_web_access (r, &s->running, &why);
  if (code != 200)
    {
      *body = text_bytes (why);
      return code;
    }
  raw = g_strsplit (r->path, "/", -1);
  for (char **p = raw; *p; p++)
    if (**p)
      {
        char *d = g_uri_unescape_string (*p, NULL);
        g_ptr_array_add (parts, d ? d : g_strdup (*p));
      }
  if (g_str_equal (r->method, "GET") && parts->len == 0)
    {
      GBytes *page = bro_web_page ();
      if (!page)
        {
          *body = text_bytes ("The page is missing from the app");
          return 404;
        }
      *content_type = "text/html; charset=utf-8";
      *body = page;
      return 200;
    }
  if (g_str_equal (r->method, "GET") && parts->len == 2 && g_str_equal (parts->pdata[0], "api") && g_str_equal (parts->pdata[1], "status"))
    {
      g_autoptr (JsonNode) node = s->status ? s->status (s->user_data) : NULL;
      g_autoptr (JsonGenerator) gen = json_generator_new ();
      gsize len = 0;
      char *text;
      if (!node)
        node = json_node_init_object (json_node_alloc (), json_object_new ());
      json_generator_set_root (gen, node);
      text = json_generator_to_data (gen, &len);
      *content_type = "application/json";
      *body = g_bytes_new_take (text, len);
      return 200;
    }
  /* POST /api/<kind>/<id>/<action>, and /api/verify (check the output folder's archives) · /api/verify/cancel. */
  if (g_str_equal (r->method, "POST") && parts->len >= 2 && parts->len <= 4 && g_str_equal (parts->pdata[0], "api")
      && (parts->len == 4 || g_str_equal (parts->pdata[1], "verify")))
    {
      const char *id = parts->len == 4 ? parts->pdata[2] : "";
      const char *action = parts->len == 4 ? parts->pdata[3] : parts->len == 3 ? parts->pdata[2] : "start";
      g_autofree char *err = s->action ? s->action (parts->pdata[1], id, action, s->user_data)
                                       : g_strdup ("Unknown action");
      if (!err)
        {
          *body = text_bytes ("");
          return 204;
        }
      *body = text_bytes (err);
      return 400;
    }
  *body = text_bytes ("Not found");
  return 404;
}

static void
on_written (GObject *stream, GAsyncResult *res, gpointer data)
{
  g_output_stream_write_all_finish (G_OUTPUT_STREAM (stream), res, NULL, NULL);
  conn_free (data);
}

static void
respond (Conn *c, const BroHttpRequest *r)
{
  const char *type = NULL;
  g_autoptr (GBytes) body = NULL;
  int code = bro_web_server_handle (c->server, r, &type, &body);
  gsize len = 0;
  const guint8 *data = g_bytes_get_data (body, &len);
  GByteArray *out = g_byte_array_new ();
  g_autofree char *head = g_strdup_printf ("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %" G_GSIZE_FORMAT "\r\n"
                                           "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n",
                                           code, reason (code), type, len);
  g_byte_array_append (out, (const guint8 *) head, strlen (head));
  if (len)
    g_byte_array_append (out, data, len);
  c->out = g_byte_array_free_to_bytes (out);
  {
    gsize n = 0;
    const void *p = g_bytes_get_data (c->out, &n);
    g_output_stream_write_all_async (g_io_stream_get_output_stream (G_IO_STREAM (c->conn)), p, n, G_PRIORITY_DEFAULT, NULL,
                                     on_written, c);
  }
}

static void
on_read (GObject *stream, GAsyncResult *res, gpointer data)
{
  Conn *c = data;
  gssize n = g_input_stream_read_finish (G_INPUT_STREAM (stream), res, NULL);
  g_autoptr (BroHttpRequest) r = NULL;
  if (n > 0)
    g_byte_array_append (c->buf, (const guint8 *) c->chunk, (guint) n);
  r = bro_http_request_parse ((const char *) c->buf->data, c->buf->len);
  if (r)
    {
      respond (c, r);
      return;
    }
  if (n <= 0 || c->buf->len > MAX_HEAD)
    {
      conn_free (c);
      return;
    }
  g_input_stream_read_async (G_INPUT_STREAM (stream), c->chunk, sizeof c->chunk, G_PRIORITY_DEFAULT, NULL, on_read, c);
}

static gboolean
on_incoming (GSocketService *service, GSocketConnection *conn, GObject *source, gpointer data)
{
  Conn *c = g_new0 (Conn, 1);
  c->server = data;
  c->conn = g_object_ref (conn);
  c->buf = g_byte_array_new ();
  g_input_stream_read_async (g_io_stream_get_input_stream (G_IO_STREAM (conn)), c->chunk, sizeof c->chunk, G_PRIORITY_DEFAULT,
                             NULL, on_read, c);
  return TRUE;
}

static gboolean
same_config (const BroWebUIConfig *a, const BroWebUIConfig *b)
{
  return a->enabled == b->enabled && a->port == b->port && g_strcmp0 (a->address, b->address) == 0 && g_strcmp0 (a->token, b->token) == 0;
}

void
bro_web_server_apply (BroWebServer *s, const BroWebUIConfig *config)
{
  g_autofree char *problem = NULL;
  g_autofree char *address = NULL;
  g_autoptr (GSocketAddress) addr = NULL;
  g_autoptr (GError) error = NULL;
  GSocketService *service;

  if (s->service && same_config (config, &s->running))
    return;
  bro_web_server_stop (s);
  if (!config->enabled)
    return;
  g_clear_pointer (&s->last_error, g_free);
  if ((problem = bro_web_start_problem (config)))
    {
      s->last_error = g_steal_pointer (&problem);
      return;
    }
  address = g_strstrip (g_strdup (config->address));
  if (g_str_equal (address, "localhost"))
    {
      g_free (address);
      address = g_strdup ("127.0.0.1");
    }
  addr = g_inet_socket_address_new_from_string (address, (guint) config->port);
  if (!addr)
    {
      s->last_error = g_strdup_printf ("Web page: “%s” is not an IP address", address);
      return;
    }
  service = g_socket_service_new ();
  if (!g_socket_listener_add_address (G_SOCKET_LISTENER (service), addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error))
    {
      s->last_error = g_strdup_printf ("Web page: %s", error->message);
      g_object_unref (service);
      return;
    }
  g_signal_connect (service, "incoming", G_CALLBACK (on_incoming), s);
  g_socket_service_start (service);
  s->service = service;
  s->running.enabled = TRUE;
  s->running.address = g_strdup (config->address);
  s->running.port = config->port;
  s->running.token = g_strdup (config->token ? config->token : "");
}
