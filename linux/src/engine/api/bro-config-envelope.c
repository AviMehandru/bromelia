/* bro-config-envelope.c */
#include "bro-config-envelope.h"

BroConfigEnvelope *
bro_config_envelope_new (gint64 version, BroConfig *config, GPtrArray *issues, GPtrArray *secrets)
{
  BroConfigEnvelope *e = g_new0 (BroConfigEnvelope, 1);
  e->version = version;
  e->config = config;
  e->issues = issues;
  e->secrets = secrets;
  return e;
}

void
bro_config_envelope_free (BroConfigEnvelope *envelope)
{
  if (!envelope)
    return;
  bro_config_free (envelope->config);
  g_ptr_array_unref (envelope->issues);
  g_ptr_array_unref (envelope->secrets);
  g_free (envelope);
}

BroJsonValue *
bro_config_envelope_json (const BroConfigEnvelope *envelope)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *issues = bro_json_value_new_array ();
  BroJsonValue *secrets = bro_json_value_new_array ();
  for (guint i = 0; i < envelope->issues->len; i++)
    bro_json_value_append (issues, bro_issue_to_json (g_ptr_array_index (envelope->issues, i)));
  for (guint i = 0; i < envelope->secrets->len; i++)
    {
      BroSecretStatus *s = g_ptr_array_index (envelope->secrets, i);
      BroJsonValue *j = bro_json_value_new_object ();
      bro_json_value_set (j, "name", bro_json_value_new_string (s->name));
      bro_json_value_set (j, "set", bro_json_value_new_bool (s->set));
      bro_json_value_append (secrets, j);
    }
  bro_json_value_set (o, "version", bro_json_value_new_integer (envelope->version));
  bro_json_value_set (o, "doc", bro_json_value_ref (envelope->config->document));
  bro_json_value_set (o, "issues", issues);
  bro_json_value_set (o, "secrets", secrets);
  return o;
}
