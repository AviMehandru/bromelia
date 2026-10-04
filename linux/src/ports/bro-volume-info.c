/* bro-volume-info.c */
#include "bro-volume-info.h"

BroVolumeInfo *
bro_volume_info_new (void)
{
  BroVolumeInfo *x = g_new0 (BroVolumeInfo, 1);
  return x;
}

void
bro_volume_info_free (BroVolumeInfo *x)
{
  if (!x)
    return;
  g_free (x->id);
  g_free (x->fs_type);
  g_free (x);
}
