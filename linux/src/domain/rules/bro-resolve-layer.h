/* bro-resolve-layer.h: a layer of BroProfileResolver, from lowest to highest. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_RESOLVE_LAYER_DEFAULTS,
  BRO_RESOLVE_LAYER_DEFAULT_PROFILE,
  BRO_RESOLVE_LAYER_DRIVE_PROFILE,
  BRO_RESOLVE_LAYER_DRIVE_OVERRIDES,
  BRO_RESOLVE_LAYER_RULE,
  BRO_RESOLVE_LAYER_SESSION,
} BroResolveLayer;

/* The wire form ("defaults"), as in shared/schema/common.json. */
const char *bro_resolve_layer_to_wire (BroResolveLayer value);
gboolean bro_resolve_layer_from_wire (const char *text, BroResolveLayer *out);

G_END_DECLS
