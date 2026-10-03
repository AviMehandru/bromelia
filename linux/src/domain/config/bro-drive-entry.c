/* bro-drive-entry.c */
#include "bro-drive-entry.h"

BroDriveEntry *
bro_drive_entry_new (const char *id, BroDriveMatch *match)
{
  BroDriveEntry *e = g_new0 (BroDriveEntry, 1);
  e->id = g_strdup (id);
  e->name = g_strdup ("Drive");
  e->enabled = TRUE;
  e->match = match ? match : bro_drive_match_new (NULL, NULL);
  e->makemkv_settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return e;
}

void
bro_drive_entry_free (BroDriveEntry *entry)
{
  if (!entry)
    return;
  g_free (entry->id);
  g_free (entry->name);
  bro_drive_match_free (entry->match);
  g_free (entry->profile);
  g_hash_table_unref (entry->makemkv_settings);
  bro_json_value_unref (entry->automation);
  g_free (entry);
}

BroDriveEntry *
bro_drive_entry_decode (BroJsonValue *json)
{
  BroJsonValue *m = bro_json_value_member (json, "match");
  BroDriveEntry *e = bro_drive_entry_new (bro_json_value_get_string (bro_json_value_member (json, "id"), ""),
                                          bro_drive_match_new (bro_json_value_get_string (bro_json_value_member (m, "driveName"), ""),
                                                               bro_json_value_get_string (bro_json_value_member (m, "devicePath"), "")));
  g_free (e->name);
  e->name = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "name"), "Drive"));
  e->enabled = bro_json_value_get_bool (bro_json_value_member (json, "enabled"), TRUE);
  e->profile = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "profile"), NULL));
  BroJsonValue *settings = bro_json_value_member (json, "makemkvSettings");
  for (guint i = 0; settings && settings->keys && i < settings->keys->len; i++)
    g_hash_table_insert (e->makemkv_settings, g_strdup (settings->keys->pdata[i]),
                         g_strdup (bro_json_value_get_string (settings->items->pdata[i], "")));
  BroJsonValue *a = bro_json_value_member (json, "automation");
  e->automation = a && a->kind != BRO_JSON_VALUE_NULL ? bro_json_value_ref (a) : NULL;
  return e;
}
