/* bro-archived-match.c */
#include "bro-archived-match.h"

BroArchivedMatch *
bro_archived_match_new (const BroId *unit_id, const char *folder, const char *day)
{
  BroArchivedMatch *m = g_new0 (BroArchivedMatch, 1);
  m->unit_id = *unit_id;
  m->folder = g_strdup (folder);
  m->day = g_strdup (day);
  return m;
}

BroArchivedMatch *
bro_archived_match_copy (const BroArchivedMatch *match)
{
  return match ? bro_archived_match_new (&match->unit_id, match->folder, match->day) : NULL;
}

void
bro_archived_match_free (BroArchivedMatch *match)
{
  if (!match)
    return;
  g_free (match->folder);
  g_free (match->day);
  g_free (match);
}

BroJsonValue *
bro_archived_match_encode (const BroArchivedMatch *match)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "unitId", bro_json_value_new_string (match->unit_id.value));
  bro_json_value_set (o, "folder", bro_json_value_new_string (match->folder));
  bro_json_value_set (o, "day", bro_json_value_new_string (match->day));
  return o;
}

BroArchivedMatch *
bro_archived_match_decode (const BroJsonValue *json)
{
  BroId id = { { 0 } };
  if (!json || json->kind != BRO_JSON_VALUE_OBJECT)
    return NULL;
  g_strlcpy (id.value, bro_json_value_get_string (bro_json_value_member (json, "unitId"), ""), sizeof id.value);
  return bro_archived_match_new (&id, bro_json_value_get_string (bro_json_value_member (json, "folder"), ""),
                                 bro_json_value_get_string (bro_json_value_member (json, "day"), ""));
}
