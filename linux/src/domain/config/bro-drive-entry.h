/* bro-drive-entry.h: a drive of the configuration (config-3.json's Drive): its profile (NULL = the default
 * profile), MakeMKV settings of its own and automation (NULL = automation.defaults). */
#pragma once

#include "bro-drive-match.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *name;
  gboolean enabled;
  BroDriveMatch *match;
  char *profile;                /* nullable */
  GHashTable *makemkv_settings; /* char * → char * */
  BroJsonValue *automation;     /* nullable */
} BroDriveEntry;

/* Takes ownership of @match. Name "Drive", enabled, no profile, settings or automation. */
BroDriveEntry *bro_drive_entry_new (const char *id, BroDriveMatch *match);
void bro_drive_entry_free (BroDriveEntry *entry);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveEntry, bro_drive_entry_free)

G_END_DECLS
