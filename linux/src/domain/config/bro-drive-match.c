/* bro-drive-match.c */
#include "bro-drive-match.h"

BroDriveMatch *
bro_drive_match_new (const char *drive_name, const char *device_path)
{
  BroDriveMatch *m = g_new0 (BroDriveMatch, 1);
  m->drive_name = g_strdup (drive_name ? drive_name : "");
  m->device_path = g_strdup (device_path ? device_path : "");
  return m;
}

void
bro_drive_match_free (BroDriveMatch *match)
{
  if (!match)
    return;
  g_free (match->drive_name);
  g_free (match->device_path);
  g_free (match);
}
