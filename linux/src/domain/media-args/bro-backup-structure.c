/* bro-backup-structure.c */
#include "bro-backup-structure.h"

#include <string.h>

static BroBroMessage *
named (BroMessageCode code, const char *name)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "name", bro_json_value_new_string (name));
  return bro_bro_message_new (code, params, BRO_SEVERITY_INFO);
}

static gboolean
has (const char *const *entries, const char *prefix)
{
  for (int i = 0; entries[i]; i++)
    if (g_ascii_strncasecmp (entries[i], prefix, strlen (prefix)) == 0)
      return TRUE;
  return FALSE;
}

BroBroMessage *
bro_backup_structure_problem (const char *name, gboolean iso, const char *const *entries, GBytes *iso_header)
{
  if (iso) {
    if (entries)
      return named (BRO_MSG_STRUCTURE_IS_FOLDER, name);
    if (!iso_header)
      return named (BRO_MSG_STRUCTURE_NOT_CREATED, name);
    gsize n;
    const char *id = g_bytes_get_data (iso_header, &n);
    if (n >= 5 && (memcmp (id, "CD001", 5) == 0 || memcmp (id, "BEA01", 5) == 0))
      return NULL;
    return named (BRO_MSG_STRUCTURE_NOT_IMAGE, name);
  }
  if (!entries)
    return named (iso_header ? BRO_MSG_STRUCTURE_NOT_FOLDER : BRO_MSG_STRUCTURE_NOT_CREATED, name);
  if (has (entries, "BDMV/"))
    return has (entries, "BDMV/index.bdmv") ? NULL : bro_bro_message_new (BRO_MSG_STRUCTURE_BDMV_INDEX_MISSING, NULL, BRO_SEVERITY_INFO);
  if (has (entries, "VIDEO_TS/"))
    return has (entries, "VIDEO_TS/VIDEO_TS.IFO") ? NULL : bro_bro_message_new (BRO_MSG_STRUCTURE_VIDEO_TS_IFO_MISSING, NULL, BRO_SEVERITY_INFO);
  if (has (entries, "HVDVD_TS/"))
    return NULL;
  return bro_bro_message_new (BRO_MSG_STRUCTURE_NO_DISC_FOLDER, NULL, BRO_SEVERITY_INFO);
}
