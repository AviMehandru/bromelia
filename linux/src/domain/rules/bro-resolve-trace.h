/* bro-resolve-trace.h: one layer of an effective profile: which layer, its profile, drive or rule id, and the profile
 * keys it set (dotted paths; the API's ResolveStep). */
#pragma once

#include "bro-resolve-layer.h"

G_BEGIN_DECLS

typedef struct {
  BroResolveLayer layer;
  char *id;   /* nullable */
  GStrv keys; /* never NULL */
} BroResolveTrace;

/* Copies @id; takes ownership of @keys. */
BroResolveTrace *bro_resolve_trace_new (BroResolveLayer layer, const char *id, GStrv keys);
void bro_resolve_trace_free (BroResolveTrace *trace);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroResolveTrace, bro_resolve_trace_free)

G_END_DECLS
