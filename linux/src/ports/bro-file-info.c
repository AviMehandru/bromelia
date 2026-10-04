/* bro-file-info.c */
#include "bro-file-info.h"

BroFileInfo *
bro_file_info_new (void)
{
  BroFileInfo *x = g_new0 (BroFileInfo, 1);
  return x;
}

void
bro_file_info_free (BroFileInfo *x)
{
  if (!x)
    return;
  g_free (x);
}
