/* bro-naming-settings.h: a profile's naming (config-3.json's naming object), with its defaults. */
#pragma once

#include "bro-conflict-policy.h"
#include "bro-json-value.h"
#include "bro-layout.h"

G_BEGIN_DECLS

#define BRO_NAMING_DEFAULT_FOLDER_TEMPLATE "{name}{discLabel? - {discLabel}}"
#define BRO_NAMING_DEFAULT_FILE_NAME_TEMPLATE \
  "{name}{episode? - {episode}}{episodeTitle? - {episodeTitle}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}"

typedef struct {
  BroLayout layout;
  char *folder_template;
  char *file_name_template;
  char *backup_subfolder;
  BroConflictPolicy conflict_policy;
} BroNamingSettings;

/* config-3.json's defaults. */
BroNamingSettings *bro_naming_settings_new (void);
void bro_naming_settings_free (BroNamingSettings *settings);

/* Naming from its JSON (a profile's naming object), with defaults for what it leaves out. */
BroNamingSettings *bro_naming_settings_decode (BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNamingSettings, bro_naming_settings_free)

G_END_DECLS
