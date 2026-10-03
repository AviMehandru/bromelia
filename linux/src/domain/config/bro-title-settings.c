/* bro-title-settings.c */
#include "bro-title-settings.h"
#include "bro-config-private.h"
#include "bro-issue.h"

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

BroTitleSettings *
bro_title_settings_decode (BroJsonValue *json)
{
  g_autoptr (GPtrArray) issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  g_autoptr (BroJsonValue) j = _bro_schema_normalize (json, _bro_schema_def ("TitleRules"), "", TRUE, issues);
  BroTitleSettings *t = bro_title_settings_new ();
#define I(k) (int) bro_json_value_get_integer (bro_json_value_member (j, (k)), 0)
#define S(field, k)                                                                                                    \
  do {                                                                                                                 \
    g_free (t->field);                                                                                                 \
    t->field = g_strdup (bro_json_value_get_string (bro_json_value_member (j, (k)), ""));                            \
  } while (0)
  bro_title_strategy_from_wire (bro_json_value_get_string (bro_json_value_member (j, "strategy"), ""), &t->strategy);
  t->longest_count = I ("longestCount");
  S (index_pattern, "indexPattern");
  bro_index_base_from_wire (bro_json_value_get_string (bro_json_value_member (j, "indexBase"), ""), &t->index_base);
  t->min_duration_seconds = I ("minDurationSeconds");
  t->max_duration_seconds = I ("maxDurationSeconds");
  t->min_chapters = I ("minChapters");
  t->max_chapters = I ("maxChapters");
  t->min_size_mb = I ("minSizeMB");
  t->max_size_mb = I ("maxSizeMB");
  S (include_pattern, "includePattern");
  S (exclude_pattern, "excludePattern");
  t->skip_duplicates = bro_json_value_get_bool (bro_json_value_member (j, "skipDuplicates"), TRUE);
  t->skip_alternate_angles = bro_json_value_get_bool (bro_json_value_member (j, "skipAlternateAngles"), FALSE);
  t->max_titles = I ("maxTitles");
#undef I
#undef S
  return t;
}
