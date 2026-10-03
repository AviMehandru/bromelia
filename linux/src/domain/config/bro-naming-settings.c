/* bro-naming-settings.c */
#include "bro-naming-settings.h"
#include "bro-config-private.h"
#include "bro-issue.h"

BroNamingSettings *
bro_naming_settings_new (void)
{
  BroNamingSettings *s = g_new0 (BroNamingSettings, 1);
  s->layout = BRO_LAYOUT_TEMPLATES;
  s->folder_template = g_strdup (BRO_NAMING_DEFAULT_FOLDER_TEMPLATE);
  s->file_name_template = g_strdup (BRO_NAMING_DEFAULT_FILE_NAME_TEMPLATE);
  s->backup_subfolder = g_strdup ("backup");
  s->conflict_policy = BRO_CONFLICT_POLICY_NEW_FOLDER;
  return s;
}

void
bro_naming_settings_free (BroNamingSettings *s)
{
  if (!s)
    return;
  g_free (s->folder_template);
  g_free (s->file_name_template);
  g_free (s->backup_subfolder);
  g_free (s);
}

BroNamingSettings *
bro_naming_settings_decode (BroJsonValue *json)
{
  g_autoptr (GPtrArray) issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  BroJsonValue *node = bro_json_value_member (bro_json_value_member (_bro_schema_def ("ProfileFields"), "properties"), "naming");
  g_autoptr (BroJsonValue) j = _bro_schema_normalize (json, node, "", TRUE, issues);
  BroNamingSettings *s = bro_naming_settings_new ();
  bro_layout_from_wire (bro_json_value_get_string (bro_json_value_member (j, "layout"), ""), &s->layout);
  g_free (s->folder_template);
  s->folder_template = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "folderTemplate"), ""));
  g_free (s->file_name_template);
  s->file_name_template = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "fileNameTemplate"), ""));
  g_free (s->backup_subfolder);
  s->backup_subfolder = g_strdup (bro_json_value_get_string (bro_json_value_member (j, "backupSubfolder"), ""));
  bro_conflict_policy_from_wire (bro_json_value_get_string (bro_json_value_member (j, "conflictPolicy"), ""), &s->conflict_policy);
  return s;
}
