/* bro-config.c */
#include "bro-config.h"
#include "bro-config-private.h"

#include <string.h>

BroConfig *
bro_config_new (BroJsonValue *document, GPtrArray *issues)
{
  BroConfig *c = g_new0 (BroConfig, 1);
  c->document = document;
  c->issues = issues ? issues : g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  return c;
}

void
bro_config_free (BroConfig *config)
{
  if (!config)
    return;
  bro_json_value_unref (config->document);
  g_ptr_array_unref (config->issues);
  g_free (config);
}

typedef gpointer (*Decode) (BroJsonValue *json);

static GPtrArray *
list_of (const BroConfig *c, const char *key, Decode decode, GDestroyNotify free_func)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (free_func);
  BroJsonValue *list = bro_json_value_member (c->document, key);
  for (guint i = 0; i < bro_json_value_length (list); i++)
    g_ptr_array_add (out, decode (bro_json_value_at (list, i)));
  return out;
}

GPtrArray *
bro_config_profiles (const BroConfig *c)
{
  return list_of (c, "profiles", (Decode) bro_profile_new, (GDestroyNotify) bro_profile_free);
}

GPtrArray *
bro_config_drives (const BroConfig *c)
{
  return list_of (c, "drives", (Decode) bro_drive_entry_decode, (GDestroyNotify) bro_drive_entry_free);
}

GPtrArray *
bro_config_steps (const BroConfig *c)
{
  return list_of (c, "steps", (Decode) bro_step_definition_decode, (GDestroyNotify) bro_step_definition_free);
}

GPtrArray *
bro_config_rules (const BroConfig *c)
{
  return list_of (c, "rules", (Decode) bro_rule_decode, (GDestroyNotify) bro_rule_free);
}

BroJsonValue *
bro_config_automation (const BroConfig *config, const char *entry_id)
{
  BroJsonValue *chosen = NULL;
  BroJsonValue *drives = bro_json_value_member (config->document, "drives");
  for (guint i = 0; entry_id && i < bro_json_value_length (drives); i++) {
    BroJsonValue *d = bro_json_value_at (drives, i);
    BroJsonValue *own = bro_json_value_member (d, "automation");
    if (g_strcmp0 (bro_json_value_get_string (bro_json_value_member (d, "id"), NULL), entry_id) == 0) {
      if (own && own->kind == BRO_JSON_VALUE_OBJECT)
        chosen = own;
      break;
    }
  }
  if (!chosen)
    chosen = bro_json_value_member (bro_json_value_member (config->document, "automation"), "defaults");
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  g_autoptr (GPtrArray) issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  return _bro_schema_normalize_full (chosen ? chosen : empty, _bro_schema_def ("DriveAutomation"), "", TRUE, TRUE, issues);
}
