/* bro-platform-http-client.h: BroPlatformHttpClient: HttpClient on libsoup 3 (plan §6, §9;
 * shared/fixtures/adapters/http-client.cases.json). Any status is an answer; no answer is http.failed with the
 * system's reason; a cancelled call is job.cancelled. Follows redirects within the same origin only (the same host and
 * port; http to https on the same host too): a redirect elsewhere is the answer, so a request's headers (an API key)
 * never go to a host the request didn't name. Bodies are read up to 16 MiB; a larger one is http.failed. Gives up
 * after the timeout (60 s), and says User-Agent: Bromelia unless the request sets one. Errors never quote the URL. Synchronous: call it from a worker thread (plan §6). Built only
 * where libsoup 3 is found (always on Linux). */
#pragma once

#include "bro-http-client.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLATFORM_HTTP_CLIENT (bro_platform_http_client_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformHttpClient, bro_platform_http_client, BRO, PLATFORM_HTTP_CLIENT, GObject)

/* @timeout_seconds 0: 60 s. */
BroPlatformHttpClient *bro_platform_http_client_new (int timeout_seconds);

BroHttpResponse *bro_platform_http_client_send (BroPlatformHttpClient *self, const BroHttpRequestSpec *request, BroCancellationToken *cancel,
                                                BroBroError **error);

G_END_DECLS
