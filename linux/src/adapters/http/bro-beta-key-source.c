/* bro-beta-key-source.c */
#include "bro-beta-key-source.h"

#include "bro-beta-key-page.h"
#include "bro-message-code.h"

struct _BroBetaKeySource {
  GObject parent_instance;
  BroHttpClient *http;
};

G_DEFINE_FINAL_TYPE (BroBetaKeySource, bro_beta_key_source, G_TYPE_OBJECT)

char *
bro_beta_key_source_current_key (BroBetaKeySource *self, BroCancellationToken *cancel, BroBroError **error)
{
  g_autoptr (BroHttpRequestSpec) request = bro_http_request_spec_new ("GET", BRO_BETA_KEY_PAGE_URL);
  g_autoptr (BroBroError) failure = NULL;
  g_autoptr (BroHttpResponse) response = bro_http_client_send (self->http, request, cancel, &failure);
  g_autofree char *html = NULL;
  char *key;
  gsize n = 0;
  const char *data;
  if (!response)
    {
      if (failure && g_str_equal (failure->code, bro_message_code_wire (BRO_MSG_HTTP_FAILED)))
        {
          BroJsonValue *params = bro_json_value_new_object ();
          BroJsonValue *reason = bro_json_value_new_object ();
          bro_json_value_set (reason, "code", bro_json_value_new_string (failure->code));
          bro_json_value_set (reason, "params", bro_json_value_ref (failure->params));
          bro_json_value_set (params, "reason", reason);
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_MAKEMKV_BETA_KEY_FETCH_FAILED), params);
        }
      else if (error)
        *error = g_steal_pointer (&failure);
      return NULL;
    }
  if (response->status != 200)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "status", bro_json_value_new_integer (response->status));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_MAKEMKV_BETA_KEY_HTTP), params);
      return NULL;
    }
  data = response->body ? g_bytes_get_data (response->body, &n) : NULL;
  html = g_strndup (data ? data : "", n);
  key = bro_beta_key_page_parse (html);
  if (!key)
    bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_MAKEMKV_BETA_KEY_NOT_FOUND), NULL);
  return key;
}

static void
bro_beta_key_source_finalize (GObject *object)
{
  g_clear_object (&BRO_BETA_KEY_SOURCE (object)->http);
  G_OBJECT_CLASS (bro_beta_key_source_parent_class)->finalize (object);
}

static void
bro_beta_key_source_class_init (BroBetaKeySourceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_beta_key_source_finalize;
}

static void
bro_beta_key_source_init (BroBetaKeySource *self)
{
}

BroBetaKeySource *
bro_beta_key_source_new (BroHttpClient *http)
{
  BroBetaKeySource *self = g_object_new (BRO_TYPE_BETA_KEY_SOURCE, NULL);
  self->http = g_object_ref (http);
  return self;
}
