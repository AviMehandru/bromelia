/* bro-makemkv-drive.c */
#include "bro-makemkv-drive.h"

BroMakemkvDrive *
bro_makemkv_drive_new (int index, BroDriveState state, int flags, const char *identification, const char *label,
                       const char *device)
{
  BroMakemkvDrive *d = g_new0 (BroMakemkvDrive, 1);
  d->index = index;
  d->state = state;
  d->flags.raw = flags;
  d->identification = g_strdup (identification ? identification : "");
  d->label = g_strdup (label ? label : "");
  d->device = g_strdup (device ? device : "");
  return d;
}

BroMakemkvDrive *
bro_makemkv_drive_copy (const BroMakemkvDrive *drive)
{
  return drive ? bro_makemkv_drive_new (drive->index, drive->state, drive->flags.raw, drive->identification, drive->label,
                                        drive->device)
               : NULL;
}

void
bro_makemkv_drive_free (BroMakemkvDrive *drive)
{
  if (!drive)
    return;
  g_free (drive->identification);
  g_free (drive->label);
  g_free (drive->device);
  g_free (drive);
}

BroMakemkvDrive *
bro_makemkv_drive_from (const BroRobotEvent *event)
{
  if (!event || event->kind != BRO_ROBOT_EVENT_DRIVE)
    return NULL;
  BroDriveState s;
  switch (event->state) {
  case 0: s = BRO_DRIVE_STATE_EMPTY_CLOSED; break;
  case 1: s = BRO_DRIVE_STATE_EMPTY_OPEN; break;
  case 2: s = BRO_DRIVE_STATE_INSERTED; break;
  case 3: s = BRO_DRIVE_STATE_LOADING; break;
  case 257: s = BRO_DRIVE_STATE_UNMOUNTING; break;
  default: s = BRO_DRIVE_STATE_NO_DRIVE; break;
  }
  return bro_makemkv_drive_new (event->index, s, event->flags, event->identification, event->label, event->device);
}

gboolean
bro_makemkv_drive_is_present (const BroMakemkvDrive *drive)
{
  return drive->state != BRO_DRIVE_STATE_NO_DRIVE && !(drive->identification[0] == '\0' && drive->device[0] == '\0');
}
