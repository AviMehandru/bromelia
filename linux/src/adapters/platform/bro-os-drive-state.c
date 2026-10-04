/* bro-os-drive-state.c */
#include "bro-os-drive-state.h"

BroOsDriveState *
bro_os_drive_state_new (BroOsDrive *drive, gboolean media)
{
  BroOsDriveState *state = g_new0 (BroOsDriveState, 1);
  state->drive = drive;
  state->media = media;
  return state;
}

void
bro_os_drive_state_free (BroOsDriveState *state)
{
  if (!state)
    return;
  bro_os_drive_free (state->drive);
  g_free (state);
}

static void
add (GPtrArray *events, BroDeviceEventKind kind, const BroOsDrive *drive, const char *device, const char *path)
{
  BroDeviceEvent *e = bro_device_event_new ();
  e->kind = kind;
  e->drive = drive ? bro_os_drive_copy (drive) : NULL;
  e->device = g_strdup (device);
  e->path = g_strdup (path);
  g_ptr_array_add (events, e);
}

static const BroOsDriveState *
find (GPtrArray *states, const char *device)
{
  for (guint i = 0; states && i < states->len; i++)
    if (g_strcmp0 (((BroOsDriveState *) states->pdata[i])->drive->device, device) == 0)
      return states->pdata[i];
  return NULL;
}

GPtrArray *
bro_os_drive_state_changes (GPtrArray *before, GPtrArray *after)
{
  GPtrArray *events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_device_event_free);
  for (guint i = 0; after && i < after->len; i++)
    {
      const BroOsDriveState *now = after->pdata[i], *was = find (before, now->drive->device);
      const char *device = now->drive->device, *path = now->drive->mount_path;
      if (!was)
        add (events, BRO_DEVICE_EVENT_DRIVE_APPEARED, now->drive, device, NULL);
      else
        {
          if (was->drive->mount_path && g_strcmp0 (was->drive->mount_path, path) != 0)
            add (events, BRO_DEVICE_EVENT_UNMOUNTED, NULL, device, NULL);
          if (was->media && !now->media)
            add (events, BRO_DEVICE_EVENT_MEDIA_REMOVED, NULL, device, NULL);
        }
      if (now->media && !(was && was->media))
        add (events, BRO_DEVICE_EVENT_MEDIA_ARRIVED, NULL, device, NULL);
      if (path && !(was && g_strcmp0 (was->drive->mount_path, path) == 0))
        add (events, BRO_DEVICE_EVENT_MOUNTED, NULL, device, path);
    }
  for (guint i = 0; before && i < before->len; i++)
    {
      const BroOsDriveState *gone = before->pdata[i];
      if (find (after, gone->drive->device))
        continue;
      if (gone->drive->mount_path)
        add (events, BRO_DEVICE_EVENT_UNMOUNTED, NULL, gone->drive->device, NULL);
      if (gone->media)
        add (events, BRO_DEVICE_EVENT_MEDIA_REMOVED, NULL, gone->drive->device, NULL);
      add (events, BRO_DEVICE_EVENT_DRIVE_VANISHED, NULL, gone->drive->device, NULL);
    }
  return events;
}
