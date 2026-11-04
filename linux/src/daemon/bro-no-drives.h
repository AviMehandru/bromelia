/* bro-no-drives.h: what the root builds when BroRootOptions.drives is FALSE: nothing asks a drive anything. */
#pragma once

#include "bro-device-monitor.h"
#include "bro-drive-control.h"
#include "bro-makemkv.h"

G_BEGIN_DECLS

/* The operating system reports no drive. */
BroDeviceMonitor *bro_no_device_monitor_new (void);

/* Every drive command is refused (drive.notFound). */
BroDriveControl *bro_no_drive_control_new (void);

/* @makemkv as it is, but its drive scan finds none and never starts makemkvcon. */
BroMakemkv *bro_no_drive_scan_new (BroMakemkv *makemkv);

G_END_DECLS
