/* bro-disc-format.h: the disc's format */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_DISC_FORMAT_DVD,
  BRO_DISC_FORMAT_BLURAY,
  BRO_DISC_FORMAT_UHD,
  BRO_DISC_FORMAT_HDDVD,
  BRO_DISC_FORMAT_UNKNOWN,
} BroDiscFormat;

/* The wire form ("dvd"), as in shared/schema/common.json. */
const char *bro_disc_format_to_wire (BroDiscFormat value);
gboolean bro_disc_format_from_wire (const char *text, BroDiscFormat *out);

G_END_DECLS
