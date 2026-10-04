/* bro-http-client.h: BroHttpClient: Sends requests that Domain built. */
#pragma once

#include "bro-bro-error.h"
#include "bro-cancellation-token.h"
#include "bro-http-request-spec.h"
#include "bro-http-response.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_HTTP_CLIENT (bro_http_client_get_type ())
G_DECLARE_INTERFACE (BroHttpClient, bro_http_client, BRO, HTTP_CLIENT, GObject)

struct _BroHttpClientInterface {
  GTypeInterface parent_iface;

  BroHttpResponse *(*send) (BroHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error);
};

/* Any status is an answer; fails when there is none. */
BroHttpResponse *bro_http_client_send (BroHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
