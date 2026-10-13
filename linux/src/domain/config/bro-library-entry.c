/* bro-library-entry.c */
#include "bro-library-entry.h"

BroLibraryEntry *
bro_library_entry_new (const char *id, const char *name, const char *path)
{
  BroLibraryEntry *e = g_new0 (BroLibraryEntry, 1);
  e->id = g_strdup (id);
  e->name = g_strdup (name);
  e->path = g_strdup (path);
  return e;
}

void
bro_library_entry_free (BroLibraryEntry *e)
{
  if (!e)
    return;
  g_free (e->id);
  g_free (e->name);
  g_free (e->path);
  g_free (e);
}

BroLibraryEntry *
bro_library_entry_decode (BroJsonValue *json)
{
  return bro_library_entry_new (bro_json_value_get_string (bro_json_value_member (json, "id"), ""),
                                bro_json_value_get_string (bro_json_value_member (json, "name"), ""),
                                bro_json_value_get_string (bro_json_value_member (json, "path"), ""));
}
