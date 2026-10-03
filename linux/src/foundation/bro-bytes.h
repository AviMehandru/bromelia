/* bro-bytes.h: a number of bytes. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gint64 count;
} BroBytes;

G_END_DECLS
