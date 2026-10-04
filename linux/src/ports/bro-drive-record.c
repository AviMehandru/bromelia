/* bro-drive-record.c */
#include "bro-drive-record.h"

BroDriveRecord *
bro_drive_record_new (void)
{
  BroDriveRecord *x = g_new0 (BroDriveRecord, 1);
  return x;
}

void
bro_drive_record_free (BroDriveRecord *x)
{
  if (!x)
    return;
  g_free (x->id);
  g_free (x->identification);
  g_free (x->model);
  g_free (x->last_device);
  g_free (x->config_id);
  g_free (x);
}
