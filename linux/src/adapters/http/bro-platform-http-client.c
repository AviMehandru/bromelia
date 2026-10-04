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

BroHttpResponse *
bro_platform_http_client_send (BroPlatformHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  /* A session per call: SoupSession's sync API belongs to the thread that made it, and calls come from workers. */
  g_autoptr (SoupSession) session = soup_session_new_with_options ("timeout", (guint) self->timeout, "user-agent", "Bromelia", NULL);
  g_autoptr (SoupMessage) message = soup_message_new (request->method, request->url);
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) soup_error = NULL;
  g_autoptr (GBytes) body = NULL;
  SoupMessageHeaders *headers;
  SoupMessageHeadersIter it;
  const char *name, *value;
  const char *content_type = NULL;
  BroHttpResponse *response;
  guint handler = 0;
  if (!message)
    {
      g_autofree char *reason = g_strdup_printf ("not a URL: %s", request->url);
      http_failed (error, reason);
      return NULL;
    }
  headers = soup_message_get_request_headers (message);
  for (guint i = 0; request->headers && i + 1 < request->headers->len; i += 2)
    {
      if (g_ascii_strcasecmp (request->headers->pdata[i], "Content-Type") == 0)
        content_type = request->headers->pdata[i + 1];
      soup_message_headers_append (headers, request->headers->pdata[i], request->headers->pdata[i + 1]);
    }
  if (request->body)
    soup_message_set_request_body_from_bytes (message, content_type, request->body);
  if (cancel)
    handler = bro_cancellation_token_on_cancel (cancel, cancel_soup, g_object_ref (cancellable), g_object_unref);
  body = soup_session_send_and_read (session, message, cancellable, &soup_error);
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
