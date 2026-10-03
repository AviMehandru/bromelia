/* bro-title-settings.c */
#include "bro-title-settings.h"

BroTitleSettings *
bro_title_settings_new (void)
{
  BroTitleSettings *s = g_new0 (BroTitleSettings, 1);
  s->strategy = BRO_TITLE_STRATEGY_ALL;
  s->longest_count = 1;
  s->index_pattern = g_strdup ("");
  s->index_base = BRO_INDEX_BASE_MAKEMKV;
  s->include_pattern = g_strdup ("");
  s->exclude_pattern = g_strdup ("");
  s->skip_duplicates = TRUE;
  return s;
}

void
bro_title_settings_free (BroTitleSettings *settings)
{
  if (!settings)
    return;
  g_free (settings->index_pattern);
  g_free (settings->include_pattern);
  g_free (settings->exclude_pattern);
  g_free (settings);
}
