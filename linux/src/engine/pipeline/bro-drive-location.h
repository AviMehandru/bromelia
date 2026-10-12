/* bro-drive-location.h: a connected drive as DriveService knows it: MakeMKV's index (none until a scan saw it), the OS
 * device ("" when unknown, as on macOS for an empty drive), the configuration entry that matches it (nullable), the
 * generation of its disc (none: no disc), the disc's label and MakeMKV's flags for it. */
#pragma once

#include "bro-disc-flags.h"

G_BEGIN_DECLS

typedef struct {
  gboolean has_makemkv_index;
  int makemkv_index;
  char *device;
  char *config_id; /* nullable */
  gboolean has_generation;
  gint64 generation;
  char *label;
  gboolean has_flags;
  BroDiscFlags flags;
} BroDriveLocation;

BroDriveLocation *bro_drive_location_new (const char *device, const char *config_id, const char *label);
BroDriveLocation *bro_drive_location_copy (const BroDriveLocation *location);
void bro_drive_location_free (BroDriveLocation *location);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveLocation, bro_drive_location_free)

G_END_DECLS
