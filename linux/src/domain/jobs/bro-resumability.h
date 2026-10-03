/* bro-resumability.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_RESUMABILITY_IDEMPOTENT,
  BRO_RESUMABILITY_RESUME_FROM,
  BRO_RESUMABILITY_NOT_RESUMABLE,
} BroResumability;

/* The wire form ("idempotent"), as in shared/schema/common.json. */
const char *bro_resumability_to_wire (BroResumability value);
gboolean bro_resumability_from_wire (const char *text, BroResumability *out);

G_END_DECLS
