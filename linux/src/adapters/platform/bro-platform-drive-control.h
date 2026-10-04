/* bro-platform-drive-control.h: BroPlatformDriveControl: DriveControl on Linux (plan §10.3;
 * shared/fixtures/adapters/drive-control.cases.json), today's code: eject and close the tray with the eject tool (it
 * unmounts first), the mount from /proc/self/mountinfo, the content from udev's database (ID_CDROM_MEDIA_*: what
 * today read with udevadm) after a mounted DVD / Blu-ray structure, raw reads of the device (BLKGETSIZE64) or of a disc
 * image file. Only a /dev path is a drive. Synchronous. */
#pragma once

#include "bro-clock.h"
#include "bro-drive-control.h"
#include "bro-process-launcher.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLATFORM_DRIVE_CONTROL (bro_platform_drive_control_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformDriveControl, bro_platform_drive_control, BRO, PLATFORM_DRIVE_CONTROL, GObject)

/* Keeps references. */
BroPlatformDriveControl *bro_platform_drive_control_new (BroProcessLauncher *launcher, BroClock *clock);

/* FALSE and @error set (drive.ejectFailed / drive.closeTrayFailed). */
gboolean bro_platform_drive_control_eject (BroPlatformDriveControl *self, const char *device, BroBroError **error);
gboolean bro_platform_drive_control_close_tray (BroPlatformDriveControl *self, const char *device, BroBroError **error);
/* The mount path (free with g_free); NULL after @timeout, when cancelled, or for something that isn't a drive. */
char *bro_platform_drive_control_wait_for_mount (BroPlatformDriveControl *self, const char *device, BroDuration timeout, BroCancellationToken *cancel);
BroDiscContent bro_platform_drive_control_probe_content (BroPlatformDriveControl *self, const char *device);
/* NULL and @error set (fs.notFound, fs.failed). */
BroSectorReader *bro_platform_drive_control_open_raw (BroPlatformDriveControl *self, const char *device, BroBroError **error);

G_END_DECLS
