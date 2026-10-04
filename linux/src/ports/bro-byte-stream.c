/* bro-byte-stream.c */
#include "bro-byte-stream.h"

G_DEFINE_INTERFACE (BroByteStream, bro_byte_stream, G_TYPE_OBJECT)

static void
bro_byte_stream_default_init (BroByteStreamInterface *iface)
{
}

GBytes *
bro_byte_stream_read (BroByteStream *self, int max_bytes, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_BYTE_STREAM (self), NULL);
  return BRO_BYTE_STREAM_GET_IFACE (self)->read (self, max_bytes, error);
}

void
bro_byte_stream_close (BroByteStream *self)
{
  g_return_if_fail (BRO_IS_BYTE_STREAM (self));
  BRO_BYTE_STREAM_GET_IFACE (self)->close (self);
}
