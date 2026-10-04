/* bro-http-response.h: an HTTP answer: status, headers in order, body. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int status;
  GPtrArray *headers; /* char *: name, value, name, value, … */
  GBytes *body;
} BroHttpResponse;

/* Empty lists, nothing else set. */
BroHttpResponse *bro_http_response_new (void);
void bro_http_response_free (BroHttpResponse *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroHttpResponse, bro_http_response_free)

G_END_DECLS
