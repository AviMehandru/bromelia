/* bro-config-templates.c */
#include "bro-config-templates.h"

BroProfile *
bro_config_templates_archive_everything (void)
{
  g_autoptr (BroJsonValue) json = bro_json_value_parse (
    "{\"name\": \"Archive everything\","
    " \"mode\": {\"default\": \"backupThenMkv\"},"
    " \"titles\": {\"strategy\": \"all\"},"
    " \"makemkv\": {\"profile\": {\"mode\": \"generated\", \"generated\": {\"selectionRule\": \"+sel:all\"}}},"
    " \"backup\": {\"format\": \"folder\", \"keepAfterMkv\": true},"
    " \"archive\": {\"checksums\": true, \"archiveRecord\": true, \"verifyRips\": true, \"writeDiscInfo\": true},"
    " \"episodes\": {\"keepPlayAll\": true}}",
    -1);
  return bro_profile_new (json);
}
