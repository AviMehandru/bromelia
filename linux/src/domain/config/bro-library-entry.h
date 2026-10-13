/* bro-library-entry.h: BroLibraryEntry: a library of the configuration (config-3.json's Library): where archives are
 * committed. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *name;
  char *path;
} BroLibraryEntry;

BroLibraryEntry *bro_library_entry_new (const char *id, const char *name, const char *path);
void bro_library_entry_free (BroLibraryEntry *entry);

/* A library entry from its JSON ("" for what it leaves out). */
BroLibraryEntry *bro_library_entry_decode (BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLibraryEntry, bro_library_entry_free)

G_END_DECLS
