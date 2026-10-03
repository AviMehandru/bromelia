/* bro-os-drive.h: an optical drive as the OS reports it (the DeviceMonitor port): its device, identification
 * (vendor and model, when the OS knows it) and where its disc is mounted. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *device;
  char *identification;
  char *mount_path; /* nullable */
} BroOsDrive;

BroOsDrive *bro_os_drive_new (const char *device, const char *identification, const char *mount_path);
BroOsDrive *bro_os_drive_copy (const BroOsDrive *drive);
void bro_os_drive_free (BroOsDrive *drive);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroOsDrive, bro_os_drive_free)

G_END_DECLS
