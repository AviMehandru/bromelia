/* bro-drive-holder.c */
#include "bro-drive-holder.h"

BroDriveHolder *
bro_drive_holder_new (const char *drive, const BroId *job)
{
  BroDriveHolder *h = g_new0 (BroDriveHolder, 1);
  h->drive = g_strdup (drive);
  h->job = *job;
  return h;
}

void
bro_drive_holder_free (BroDriveHolder *holder)
{
  if (!holder)
    return;
  g_free (holder->drive);
  g_free (holder);
}
