/* bro-http-response.c */
#include "bro-http-response.h"

BroHttpResponse *
bro_http_response_new (void)
{
  BroHttpResponse *x = g_new0 (BroHttpResponse, 1);
  x->headers = g_ptr_array_new_with_free_func (g_free);
  return x;
}

void
bro_http_response_free (BroHttpResponse *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->headers, g_ptr_array_unref);
  g_clear_pointer (&x->body, g_bytes_unref);
  g_free (x);
}
