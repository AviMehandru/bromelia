/* bro-archive-check.c */
#include "bro-archive-check.h"
#include "bro-config.h"
#include "bro-instant.h"
#include "bro-unit-record.h"

#include <string.h>

BroArchivedMatch *
bro_archive_check_find (const BroStepContext *context, const char *fingerprint, BroBroError **error)
{
  g_autoptr (GPtrArray) units = NULL;
  if (!fingerprint)
    return NULL;
  units = bro_catalog_queries_archived_before (context->catalog, fingerprint, error);
  if (!units)
    return NULL;
  for (guint i = 0; i < units->len; i++)
    {
      const BroUnitRecord *unit = g_ptr_array_index (units, i);
      g_autoptr (BroBroError) offline = NULL;
      g_autoptr (BroLibraryHandle) library = NULL;
      g_autofree char *root = NULL;
      g_autofree char *folder = NULL;
      g_autofree char *when = NULL;
      g_autofree char *day = NULL;
      gsize n;
      if (unit->status != BRO_UNIT_STATUS_SUCCESS)
        continue;
      library = bro_library_port_require (context->library, unit->library_id, &offline);
      if (!library)
        continue;
      root = g_strdup (library->path);
      n = strlen (root);
      while (n > 0 && root[n - 1] == '/')
        root[--n] = '\0';
      folder = g_strconcat (root, "/", unit->path, NULL);
      if (!bro_file_system_exists (context->fs, folder))
        continue;
      when = bro_instant_format (unit->has_committed_at ? unit->committed_at : unit->created_at);
      day = g_strndup (when, 10);
      return bro_archived_match_new (&unit->id, folder, day);
    }
  return NULL;
}

BroBroMessage *
bro_archive_check_message (BroMessageCode code, const BroArchivedMatch *match, gboolean with_unit)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "folder", bro_json_value_new_string (match->folder));
  bro_json_value_set (params, "day", bro_json_value_new_string (match->day));
  if (with_unit)
    bro_json_value_set (params, "unitId", bro_json_value_new_string (match->unit_id.value));
  return bro_bro_message_new (code, params, BRO_SEVERITY_WARNING);
}

/* automation.alreadyArchived of an automatic rip; ripAgain for one started by hand. Free with g_free. */
static char *
policy (const BroStepContext *context, const char *config_id)
{
  g_autoptr (BroJsonValue) automation = NULL;
  if (!context->job->request->automatic)
    return g_strdup ("ripAgain");
  automation = bro_config_automation (context->config, config_id);
  return g_strdup (bro_json_value_get_string (bro_json_value_member (automation, "alreadyArchived"), ""));
}

BroBroMessage *
bro_archive_check_apply (const BroStepContext *context, const BroArchivedMatch *match, const char *config_id)
{
  g_autofree char *p = policy (context, config_id);
  if (strcmp (p, "skip") == 0)
    return bro_archive_check_message (BRO_MSG_DISC_ALREADY_ARCHIVED, match, TRUE);
  if (strcmp (p, "ask") != 0)
    {
      g_autoptr (BroBroMessage) line = bro_archive_check_message (BRO_MSG_DISC_ARCHIVED_BEFORE, match, FALSE);
      bro_job_logger_log (context->log, line);
    }
  return NULL;
}

gboolean
bro_archive_check_asks (const BroStepContext *context, const char *config_id)
{
  g_autofree char *p = policy (context, config_id);
  return strcmp (p, "ask") == 0;
}
