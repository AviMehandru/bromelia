/* bro-library-dto.c */
#include "bro-library-dto.h"
#include "bro-severity.h"

BroLibraryDto *
bro_library_dto_new (void)
{
  return g_new0 (BroLibraryDto, 1);
}

void
bro_library_dto_free (BroLibraryDto *l)
{
  if (!l)
    return;
  g_free (l->id);
  g_free (l->name);
  g_free (l->path);
  bro_bro_message_free (l->reason);
  g_free (l);
}

BroJsonValue *
bro_library_dto_json (const BroLibraryDto *l)
{
  BroJsonValue *m = bro_json_value_new_object ();
  bro_json_value_set (m, "id", bro_json_value_new_string (l->id));
  bro_json_value_set (m, "name", bro_json_value_new_string (l->name));
  bro_json_value_set (m, "path", bro_json_value_new_string (l->path));
  bro_json_value_set (m, "online", bro_json_value_new_bool (l->online));
  if (l->reason)
    {
      BroJsonValue *reason = bro_bro_message_to_json (l->reason);
      bro_json_value_set (reason, "severity", bro_json_value_new_string (bro_severity_to_wire (l->reason->severity)));
      bro_json_value_set (m, "reason", reason);
    }
  if (l->has_free_bytes)
    bro_json_value_set (m, "freeBytes", bro_json_value_new_integer (l->free_bytes));
  if (l->has_last_seen_at)
    {
      g_autofree char *seen = bro_instant_format (l->last_seen_at);
      bro_json_value_set (m, "lastSeenAt", bro_json_value_new_string (seen));
    }
  return m;
}
