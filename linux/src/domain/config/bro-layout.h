/* bro-layout.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_LAYOUT_TEMPLATES,
  BRO_LAYOUT_MEDIA_SERVER,
} BroLayout;

/* The wire form ("templates"), as in shared/schema/common.json. */
const char *bro_layout_to_wire (BroLayout value);
gboolean bro_layout_from_wire (const char *text, BroLayout *out);

G_END_DECLS
