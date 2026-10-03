/* bro-sum-entry.c */
#include "bro-sum-entry.h"

BroSumEntry *
bro_sum_entry_new (const char *path, const char *sha256)
{
  BroSumEntry *e = g_new0 (BroSumEntry, 1);
  e->path = g_strdup (path);
  e->sha256 = g_strdup (sha256);
  return e;
}

void
bro_sum_entry_free (BroSumEntry *entry)
{
  if (!entry)
    return;
  g_free (entry->path);
  g_free (entry->sha256);
  g_free (entry);
}
