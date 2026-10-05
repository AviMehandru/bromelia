/* bro-platform-http-client.c */
#include "bro-platform-http-client.h"

#include "bro-message-code.h"
#include <libsoup/soup.h>

struct _BroPlatformHttpClient {
  GObject parent_instance;
  int timeout;
};

static void bro_platform_http_client_iface_init (BroHttpClientInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformHttpClient, bro_platform_http_client, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_HTTP_CLIENT, bro_platform_http_client_iface_init))

static void
cancel_soup (gpointer cancellable, gpointer unused)
{
  g_cancellable_cancel (cancellable);
}

static void
http_failed (BroBroError **error, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_HTTP_FAILED), params);
}

#define MAX_BODY (16 * 1024 * 1024)
#define MAX_REDIRECTS 10

static int
default_port (const char *scheme)
{
  return g_strcmp0 (scheme, "https") == 0 ? 443 : 80;
}

/* Whether a redirect from @from may go to @to: the same host and port, or http to https on the same host with the
 * default ports. */
static gboolean
same_origin (GUri *from, GUri *to)
{
  const char *a = g_uri_get_scheme (from), *b = g_uri_get_scheme (to);
  int pa = g_uri_get_port (from) > 0 ? g_uri_get_port (from) : default_port (a);
  int pb = g_uri_get_port (to) > 0 ? g_uri_get_port (to) : default_port (b);
  if (g_ascii_strcasecmp (g_uri_get_host (from) ? g_uri_get_host (from) : "", g_uri_get_host (to) ? g_uri_get_host (to) : "") != 0)
    return FALSE;
  if (g_ascii_strcasecmp (a, b) == 0)
    return pa == pb;
  return g_ascii_strcasecmp (a, "http") == 0 && g_ascii_strcasecmp (b, "https") == 0 && pa == 80 && pb == 443;
}

static SoupMessage *
build (const BroHttpRequestSpec *request, const char *method, GUri *uri, GBytes *body)
{
  SoupMessage *message = soup_message_new_from_uri (method, uri);
  SoupMessageHeaders *headers = soup_message_get_request_headers (message);
  const char *content_type = NULL;
  soup_message_add_flags (message, SOUP_MESSAGE_NO_REDIRECT);
  for (guint i = 0; request->headers && i + 1 < request->headers->len; i += 2)
    {
      if (!body && g_ascii_strncasecmp (request->headers->pdata[i], "Content-", 8) == 0)
        continue;
      if (g_ascii_strcasecmp (request->headers->pdata[i], "Content-Type") == 0)
        content_type = request->headers->pdata[i + 1];
      soup_message_headers_append (headers, request->headers->pdata[i], request->headers->pdata[i + 1]);
    }
  if (body)
    soup_message_set_request_body_from_bytes (message, content_type, body);
  return message;
}

/* The body, at most MAX_BODY bytes; NULL and @error set otherwise. */
static GBytes *
read_capped (GInputStream *stream, goffset declared, GCancellable *cancellable, GError **error)
{
  g_autoptr (GByteArray) all = g_byte_array_new ();
  guint8 chunk[65536];
  gssize n;
  if (declared > MAX_BODY)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "the answer is larger than %d MiB", MAX_BODY / 1024 / 1024);
      return NULL;
    }
  while ((n = g_input_stream_read (stream, chunk, sizeof chunk, cancellable, error)) > 0)
    {
      if (all->len + (gsize) n > MAX_BODY)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "the answer is larger than %d MiB", MAX_BODY / 1024 / 1024);
          return NULL;
        }
      g_byte_array_append (all, chunk, (guint) n);
    }
  if (n < 0)
    return NULL;
  return g_byte_array_free_to_bytes (g_steal_pointer (&all));
}

BroHttpResponse *
bro_platform_http_client_send (BroPlatformHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  /* A session per call: SoupSession's sync API belongs to the thread that made it, and calls come from workers. */
  g_autoptr (SoupSession) session = soup_session_new_with_options ("timeout", (guint) self->timeout, "user-agent", "Bromelia", NULL);
  g_autoptr (GUri) uri = g_uri_parse (request->url, SOUP_HTTP_URI_FLAGS, NULL);
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) soup_error = NULL;
  g_autoptr (SoupMessage) message = NULL;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GBytes) sent_body = request->body ? g_bytes_ref (request->body) : NULL;
  const char *method = request->method;
  SoupMessageHeadersIter it;
  const char *name, *value;
  BroHttpResponse *response;
  guint handler = 0;
  if (!uri || !g_uri_get_host (uri) || (g_ascii_strcasecmp (g_uri_get_scheme (uri), "http") != 0 && g_ascii_strcasecmp (g_uri_get_scheme (uri), "https") != 0))
    {
      http_failed (error, "not a valid http(s) URL");
      return NULL;
    }
  if (cancel)
    handler = bro_cancellation_token_on_cancel (cancel, cancel_soup, g_object_ref (cancellable), g_object_unref);
  for (int hops = 0;; hops++)
    {
      g_autoptr (GInputStream) stream = NULL;
      guint status;
      const char *location;
      g_clear_object (&message);
      message = build (request, method, uri, sent_body);
      stream = soup_session_send (session, message, cancellable, &soup_error);
      if (!stream)
        break;
      status = soup_message_get_status (message);
      location = soup_message_headers_get_one (soup_message_get_response_headers (message), "Location");
      if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location && hops < MAX_REDIRECTS)
        {
          g_autoptr (GUri) next = g_uri_parse_relative (uri, location, SOUP_HTTP_URI_FLAGS, NULL);
          if (next && same_origin (uri, next))
            {
              if (status == 303 || ((status == 301 || status == 302) && g_strcmp0 (method, "POST") == 0))
                {
                  method = "GET";
                  g_clear_pointer (&sent_body, g_bytes_unref);
                }
              g_input_stream_close (stream, NULL, NULL);
              g_clear_pointer (&uri, g_uri_unref);
              uri = g_steal_pointer (&next);
              continue;
            }
        }
      body = read_capped (stream, soup_message_headers_get_content_length (soup_message_get_response_headers (message)), cancellable, &soup_error);
      break;
    }
  if (handler)
    bro_cancellation_token_disconnect (cancel, handler);
  if (!body)
    {
      if ((cancel && bro_cancellation_token_is_cancelled (cancel)) || g_error_matches (soup_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      else
        http_failed (error, soup_error ? soup_error->message : "no answer");
      return NULL;
    }
  response = bro_http_response_new ();
  response->status = (int) soup_message_get_status (message);
  g_clear_pointer (&response->body, g_bytes_unref);
  response->body = g_bytes_ref (body);
  soup_message_headers_iter_init (&it, soup_message_get_response_headers (message));
  while (soup_message_headers_iter_next (&it, &name, &value))
    {
      g_ptr_array_add (response->headers, g_strdup (name));
      g_ptr_array_add (response->headers, g_strdup (value));
    }
  return response;
}

static BroHttpResponse *
send_vfunc (BroHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  return bro_platform_http_client_send (BRO_PLATFORM_HTTP_CLIENT (self), request, cancel, error);
}

static void
bro_platform_http_client_class_init (BroPlatformHttpClientClass *klass)
{
}

static void
bro_platform_http_client_init (BroPlatformHttpClient *self)
{
}

static void
bro_platform_http_client_iface_init (BroHttpClientInterface *iface)
{
  iface->send = send_vfunc;
}

BroPlatformHttpClient *
bro_platform_http_client_new (int timeout_seconds)
{
  BroPlatformHttpClient *self = g_object_new (BRO_TYPE_PLATFORM_HTTP_CLIENT, NULL);
  self->timeout = timeout_seconds > 0 ? timeout_seconds : 60;
  return self;
}
