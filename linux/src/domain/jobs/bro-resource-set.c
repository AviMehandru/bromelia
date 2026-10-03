/* bro-resource-set.c */
#include "bro-resource-set.h"

BroResourceSet *
bro_resource_set_new (void)
{
  return g_new0 (BroResourceSet, 1);
}

void
bro_resource_set_free (BroResourceSet *set)
{
  if (!set)
    return;
  g_strfreev (set->drives);
  g_strfreev (set->library_writes);
  g_strfreev (set->io);
  g_free (set);
}
