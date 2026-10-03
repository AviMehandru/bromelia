/* bro-resolve-trace.c */
#include "bro-resolve-trace.h"

BroResolveTrace *
bro_resolve_trace_new (BroResolveLayer layer, const char *id, GStrv keys)
{
  BroResolveTrace *t = g_new0 (BroResolveTrace, 1);
  t->layer = layer;
  t->id = g_strdup (id);
  t->keys = keys ? keys : g_new0 (char *, 1);
  return t;
}

void
bro_resolve_trace_free (BroResolveTrace *trace)
{
  if (!trace)
    return;
  g_free (trace->id);
  g_strfreev (trace->keys);
  g_free (trace);
}
