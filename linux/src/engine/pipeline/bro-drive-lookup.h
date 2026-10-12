/* bro-drive-lookup.h: BroDriveLookup: where a drive is now (implemented by DriveService): MakeMKV numbers drives by scan
 * order and the device can change, so a step asks when it runs. Thread-safe. */
#pragma once

#include "bro-drive-location.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_DRIVE_LOOKUP (bro_drive_lookup_get_type ())
G_DECLARE_INTERFACE (BroDriveLookup, bro_drive_lookup, BRO, DRIVE_LOOKUP, GObject)

struct _BroDriveLookupInterface {
  GTypeInterface parent_iface;
  BroDriveLocation *(*find) (BroDriveLookup *self, const char *drive_id);
};

/* NULL when no such drive is connected; the caller frees it. */
BroDriveLocation *bro_drive_lookup_find (BroDriveLookup *self, const char *drive_id);

G_END_DECLS
