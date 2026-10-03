/* bro-config-codec.c */
#include "bro-config-codec.h"
#include "bro-bro-message.h"
#include "bro-config-private.h"

BroConfig *
bro_config_codec_decode (const char *bytes, gssize length, BroBroError **error)
{
  g_autoptr (BroJsonValue) doc = bro_json_value_parse (bytes, length);
  if (!doc || doc->kind != BRO_JSON_VALUE_OBJECT) {
    BroJsonValue *p = bro_json_value_new_object ();
    bro_json_value_set (p, "count", bro_json_value_new_integer (1));
    g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_CONFIG_REJECTED, p, BRO_SEVERITY_ERROR);
    if (error)
      *error = bro_bro_message_to_error (m, NULL);
    return NULL;
  }
  GPtrArray *issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  BroJsonValue *normalized = _bro_schema_normalize (doc, _bro_schema_document (), "", TRUE, issues);
  return bro_config_new (normalized, issues);
}

GBytes *
bro_config_codec_encode (const BroConfig *config)
{
  return bro_json_value_encode_canonical (config->document);
}

GBytes *
bro_config_codec_export_bundle (const BroConfig *config)
{
  g_autoptr (BroJsonValue) b = bro_json_value_new_object ();
  bro_json_value_set (b, "format", bro_json_value_new_string ("bromelia-config"));
  bro_json_value_set (b, "version", bro_json_value_new_integer (3));
  bro_json_value_set (b, "config", bro_json_value_ref (config->document));
  return bro_json_value_encode_canonical (b);
}
