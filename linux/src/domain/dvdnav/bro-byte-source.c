/* bro-byte-source.c */
#include "bro-byte-source.h"

GBytes *
bro_byte_source_read (BroByteSource *source, const char *path, gint64 offset, gsize length)
{
  GBytes *b = source->read (source, path, offset, length);
  return b ? b : g_bytes_new (NULL, 0);
}

GPtrArray *
bro_byte_source_files (BroByteSource *source)
{
  return source->files (source);
}
