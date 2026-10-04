/* bro-directory-entry.h: an entry of a folder. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name;
  gboolean is_directory;
} BroDirectoryEntry;

/* Everything zero. */
BroDirectoryEntry *bro_directory_entry_new (void);
void bro_directory_entry_free (BroDirectoryEntry *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDirectoryEntry, bro_directory_entry_free)

G_END_DECLS
