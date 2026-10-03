/* bro-joined-drive.c */
#include "bro-joined-drive.h"

BroJoinedDrive *
bro_joined_drive_new (const char *drive_id, BroMakemkvDrive *makemkv, BroOsDrive *os)
{
  BroJoinedDrive *d = g_new0 (BroJoinedDrive, 1);
  d->drive_id = g_strdup (drive_id);
  d->makemkv = makemkv;
  d->os = os;
  return d;
}

void
bro_joined_drive_free (BroJoinedDrive *drive)
{
  if (!drive)
    return;
  g_free (drive->drive_id);
  bro_makemkv_drive_free (drive->makemkv);
  bro_os_drive_free (drive->os);
  g_free (drive);
}
