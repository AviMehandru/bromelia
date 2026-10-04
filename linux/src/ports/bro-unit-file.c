/* bro-unit-file.c */
#include "bro-unit-file.h"

BroUnitFile *
bro_unit_file_new (void)
{
  BroUnitFile *x = g_new0 (BroUnitFile, 1);
  return x;
}

void
bro_unit_file_free (BroUnitFile *x)
{
  if (!x)
    return;
  g_free (x->path);
  g_free (x->sha256);
  g_free (x->role);
  g_free (x);
}
