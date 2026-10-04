/* bro-sector-reader.h: BroSectorReader: Raw reads of a disc (data discs, rescue). */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SECTOR_READER (bro_sector_reader_get_type ())
G_DECLARE_INTERFACE (BroSectorReader, bro_sector_reader, BRO, SECTOR_READER, GObject)

struct _BroSectorReaderInterface {
  GTypeInterface parent_iface;

  GBytes *(*read) (BroSectorReader *self, gint64 sector, int count, BroBroError **error);
  gint64 (*sector_count) (BroSectorReader *self, BroBroError **error);
  void (*close) (BroSectorReader *self);
};

/* count 2048-byte sectors from sector; fails on a read error. */
GBytes *bro_sector_reader_read (BroSectorReader *self, gint64 sector, int count, BroBroError **error);

/* -1 and @error set on failure. */
gint64 bro_sector_reader_sector_count (BroSectorReader *self, BroBroError **error);

void bro_sector_reader_close (BroSectorReader *self);

G_END_DECLS
