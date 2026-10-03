/* bro-http-request-spec.c */
#include "bro-http-request-spec.h"

BroHttpRequestSpec *
bro_http_request_spec_new (const char *method, const char *url)
{
  BroHttpRequestSpec *s = g_new0 (BroHttpRequestSpec, 1);
  s->method = g_strdup (method);
  s->url = g_strdup (url);
  s->headers = g_ptr_array_new_with_free_func (g_free);
  return s;
}

void
bro_http_request_spec_free (BroHttpRequestSpec *spec)
{
  if (!spec)
    return;
  g_free (spec->method);
  g_free (spec->url);
  g_ptr_array_unref (spec->headers);
  if (spec->body)
    g_bytes_unref (spec->body);
  g_free (spec);
}

void
bro_http_request_spec_add_header (BroHttpRequestSpec *spec, const char *name, const char *value)
{
  g_ptr_array_add (spec->headers, g_strdup (name));
  g_ptr_array_add (spec->headers, g_strdup (value));
}
