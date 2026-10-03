/* bro-drive-join.h: BroDriveJoin, MakeMKV's drives ⇄ the OS's drives and the configuration's entries. */
#pragma once

#include "bro-drive-entry.h"
#include "bro-joined-drive.h"

G_BEGIN_DECLS

/* One entry per present MakeMKV drive (BroMakemkvDrive *), with the OS drive (BroOsDrive *) of the same device;
 * then the OS drives MakeMKV didn't report (their id comes from the device until MakeMKV names them). Returns
 * BroJoinedDrive *, owning copies. */
GPtrArray *bro_drive_join_join (GPtrArray *makemkv_drives, GPtrArray *os_drives);

/* "drv-" and the first 16 hex digits of the SHA-256 of the normalised identification (lower case, runs of
 * spaces collapsed), or of "dev:<device>" when it's empty. Free with g_free. */
char *bro_drive_join_drive_id (const char *identification, const char *device);

/* drive_name compared case- and space-insensitively with the identification; device_path (ignoring case) when
 * drive_name is empty. */
gboolean bro_drive_join_matches (const BroDriveMatch *match, const BroMakemkvDrive *drive);

/* The first enabled entry (BroDriveEntry *) that matches, else the first that matches; borrowed, or NULL. */
BroDriveEntry *bro_drive_join_entry_for (GPtrArray *drives, const BroMakemkvDrive *drive);

/* The model part of an identification: "BD-RE NEW DRIVE 3.00 SN" → "NEW DRIVE 3.00". Free with g_free. */
char *bro_drive_join_short_model (const char *identification);

G_END_DECLS
