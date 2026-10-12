/* bro-drive-location.c */
#include "bro-drive-location.h"

BroDriveLocation *
bro_drive_location_new (const char *device, const char *config_id, const char *label)
{
  BroDriveLocation *l = g_new0 (BroDriveLocation, 1);
  l->device = g_strdup (device ? device : "");
  l->config_id = g_strdup (config_id);
  l->label = g_strdup (label ? label : "");
  return l;
}

BroDriveLocation *
bro_drive_location_copy (const BroDriveLocation *location)
{
  if (!location)
    return NULL;
  BroDriveLocation *l = g_new0 (BroDriveLocation, 1);
  *l = *location;
  l->device = g_strdup (location->device);
  l->config_id = g_strdup (location->config_id);
  l->label = g_strdup (location->label);
  return l;
}

void
bro_drive_location_free (BroDriveLocation *location)
{
  if (!location)
    return;
  g_free (location->device);
  g_free (location->config_id);
  g_free (location->label);
  g_free (location);
}
