/* bro-queue.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_QUEUE_ACQUISITION,
  BRO_QUEUE_PROCESSING,
  BRO_QUEUE_MAINTENANCE,
} BroQueue;

/* The wire form ("acquisition"), as in shared/schema/common.json. */
const char *bro_queue_to_wire (BroQueue value);
gboolean bro_queue_from_wire (const char *text, BroQueue *out);

G_END_DECLS
