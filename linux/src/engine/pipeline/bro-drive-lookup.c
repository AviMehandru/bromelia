/* bro-drive-lookup.c */
#include "bro-drive-lookup.h"

G_DEFINE_INTERFACE (BroDriveLookup, bro_drive_lookup, G_TYPE_OBJECT)

static void
bro_drive_lookup_default_init (BroDriveLookupInterface *iface)
{
}

BroDriveLocation *
bro_drive_lookup_find (BroDriveLookup *self, const char *drive_id)
{
  return BRO_DRIVE_LOOKUP_GET_IFACE (self)->find (self, drive_id);
}
