/* bro-byte-stream.h: BroByteStream: A file read as a stream (hashing). */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_BYTE_STREAM (bro_byte_stream_get_type ())
G_DECLARE_INTERFACE (BroByteStream, bro_byte_stream, BRO, BYTE_STREAM, GObject)

struct _BroByteStreamInterface {
  GTypeInterface parent_iface;

  GBytes *(*read) (BroByteStream *self, int max_bytes, BroBroError **error);
  void (*close) (BroByteStream *self);
};

/* Up to maxBytes; empty at the end. */
GBytes *bro_byte_stream_read (BroByteStream *self, int max_bytes, BroBroError **error);

/* Closes it. */
void bro_byte_stream_close (BroByteStream *self);

G_END_DECLS
