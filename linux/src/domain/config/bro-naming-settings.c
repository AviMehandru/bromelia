/* bro-naming-settings.c */
#include "bro-naming-settings.h"

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
