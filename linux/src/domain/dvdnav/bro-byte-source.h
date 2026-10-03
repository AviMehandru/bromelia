/* bro-byte-source.h: the one input interface defined in Domain: the files of a VIDEO_TS folder, wherever they are
 * (an ISO, a folder or a disc). Adapters implement it over FileSystem or SectorReader by putting a BroByteSource
 * first in their own struct and filling in both functions. */
#pragma once

#include "bro-byte-file.h"

G_BEGIN_DECLS

typedef struct _BroByteSource BroByteSource;

struct _BroByteSource {
  GBytes *(*read) (BroByteSource *source, const char *path, gint64 offset, gsize length);
  GPtrArray *(*files) (BroByteSource *source);
};

/* @length bytes of @path (an upper-case file name such as VTS_01_0.IFO) from byte @offset; fewer at the end of the
 * file, none when it can't be read. Never NULL. */
GBytes *bro_byte_source_read (BroByteSource *source, const char *path, gint64 offset, gsize length);

/* Every file (BroByteFile *), by name. */
GPtrArray *bro_byte_source_files (BroByteSource *source);

G_END_DECLS
