/* bro-disc-content.h: what is on a disc (DriveControl.probeContent). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_DISC_CONTENT_VIDEO,
  BRO_DISC_CONTENT_AUDIO,
  BRO_DISC_CONTENT_DATA,
  BRO_DISC_CONTENT_BLANK,
  BRO_DISC_CONTENT_UNKNOWN,
} BroDiscContent;

/* The wire form ("video"), as in shared/schema/common.json. */
const char *bro_disc_content_to_wire (BroDiscContent value);
gboolean bro_disc_content_from_wire (const char *text, BroDiscContent *out);

G_END_DECLS
