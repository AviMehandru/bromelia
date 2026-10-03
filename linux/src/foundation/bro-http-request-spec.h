/* bro-http-request-spec.h: a request built by Domain and sent by the HttpClient port: method, URL, headers in
 * order, body. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *method;
  char *url;
  GPtrArray *headers; /* char *: name, value, name, value, … */
  GBytes *body;       /* nullable */
} BroHttpRequestSpec;

BroHttpRequestSpec *bro_http_request_spec_new (const char *method, const char *url);
void bro_http_request_spec_free (BroHttpRequestSpec *spec);
void bro_http_request_spec_add_header (BroHttpRequestSpec *spec, const char *name, const char *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroHttpRequestSpec, bro_http_request_spec_free)

G_END_DECLS
