/* bro-drive-stats.c */
#include "bro-drive-stats.h"

BroDriveStats *
bro_drive_stats_new (void)
{
  BroDriveStats *x = g_new0 (BroDriveStats, 1);
  return x;
}

void
bro_drive_stats_free (BroDriveStats *x)
{
  if (!x)
    return;
  g_free (x);
}
