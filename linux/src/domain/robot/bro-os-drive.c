/* bro-os-drive.c */
#include "bro-os-drive.h"

BroOsDrive *
bro_os_drive_new (const char *device, const char *identification, const char *mount_path)
{
  BroOsDrive *d = g_new0 (BroOsDrive, 1);
  d->device = g_strdup (device ? device : "");
  d->identification = g_strdup (identification ? identification : "");
  d->mount_path = g_strdup (mount_path);
  return d;
}

BroOsDrive *
bro_os_drive_copy (const BroOsDrive *drive)
{
  return drive ? bro_os_drive_new (drive->device, drive->identification, drive->mount_path) : NULL;
}

void
bro_os_drive_free (BroOsDrive *drive)
{
  if (!drive)
    return;
  g_free (drive->device);
  g_free (drive->identification);
  g_free (drive->mount_path);
  g_free (drive);
}
