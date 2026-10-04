/* bro-directory-entry.c */
#include "bro-directory-entry.h"

BroDirectoryEntry *
bro_directory_entry_new (void)
{
  BroDirectoryEntry *x = g_new0 (BroDirectoryEntry, 1);
  return x;
}

void
bro_directory_entry_free (BroDirectoryEntry *x)
{
  if (!x)
    return;
  g_free (x->name);
  g_free (x);
}
