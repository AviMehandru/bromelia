/* bro-sum-entry.h: a line of SHA256SUMS: a path relative to the unit's folder (folders separated by '/') and its
 * SHA-256 in lower-case hex. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *path;
  char *sha256;
} BroSumEntry;

BroSumEntry *bro_sum_entry_new (const char *path, const char *sha256);
void bro_sum_entry_free (BroSumEntry *entry);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSumEntry, bro_sum_entry_free)

G_END_DECLS
