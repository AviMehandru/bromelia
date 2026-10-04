/* bro-sector-reader.c */
#include "bro-sector-reader.h"

G_DEFINE_INTERFACE (BroSectorReader, bro_sector_reader, G_TYPE_OBJECT)

static void
bro_sector_reader_default_init (BroSectorReaderInterface *iface)
{
}

GBytes *
bro_sector_reader_read (BroSectorReader *self, gint64 sector, int count, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_SECTOR_READER (self), NULL);
  return BRO_SECTOR_READER_GET_IFACE (self)->read (self, sector, count, error);
}

gint64
bro_sector_reader_sector_count (BroSectorReader *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_SECTOR_READER (self), -1);
  return BRO_SECTOR_READER_GET_IFACE (self)->sector_count (self, error);
}

void
bro_sector_reader_close (BroSectorReader *self)
{
  g_return_if_fail (BRO_IS_SECTOR_READER (self));
  BRO_SECTOR_READER_GET_IFACE (self)->close (self);
}
