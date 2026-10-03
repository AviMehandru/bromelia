/* bro-disc-type.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_DISC_TYPE_DVD,
  BRO_DISC_TYPE_BD,
  BRO_DISC_TYPE_HDDVD,
  BRO_DISC_TYPE_DISC,
} BroDiscType;

/* The wire form ("dvd"), as in shared/schema/common.json. */
const char *bro_disc_type_to_wire (BroDiscType value);
gboolean bro_disc_type_from_wire (const char *text, BroDiscType *out);

G_END_DECLS
