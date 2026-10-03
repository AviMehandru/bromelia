/* bro-title-settings.h: a profile's title rules (config-3.json's TitleRules object; BroTitleRules applies them):
 * filters (duration, chapters, size, patterns, angles), duplicate removal, the strategy, then max_titles. */
#pragma once

#include "bro-index-base.h"
#include "bro-title-strategy.h"

G_BEGIN_DECLS

typedef struct {
  BroTitleStrategy strategy;
  int longest_count;
  char *index_pattern;
  BroIndexBase index_base;
  int min_duration_seconds;
  int max_duration_seconds;
  int min_chapters;
  int max_chapters;
  int min_size_mb;
  int max_size_mb;
  char *include_pattern;
  char *exclude_pattern;
  gboolean skip_duplicates;
  gboolean skip_alternate_angles;
  int max_titles;
} BroTitleSettings;

/* config-3.json's defaults. */
BroTitleSettings *bro_title_settings_new (void);
void bro_title_settings_free (BroTitleSettings *settings);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroTitleSettings, bro_title_settings_free)

G_END_DECLS
