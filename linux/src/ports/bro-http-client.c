/* bro-http-client.c */
#include "bro-http-client.h"

G_DEFINE_INTERFACE (BroHttpClient, bro_http_client, G_TYPE_OBJECT)

static void
bro_http_client_default_init (BroHttpClientInterface *iface)
{
}

BroHttpResponse *
bro_http_client_send (BroHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_HTTP_CLIENT (self), NULL);
  return BRO_HTTP_CLIENT_GET_IFACE (self)->send (self, request, cancel, error);
}
