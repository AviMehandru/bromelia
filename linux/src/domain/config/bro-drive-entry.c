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
