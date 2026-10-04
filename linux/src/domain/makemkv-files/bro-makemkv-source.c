/* bro-makemkv-source.c */
#include "bro-makemkv-source.h"

BroMakemkvSource *
bro_makemkv_source_new (BroMakemkvSourceKind kind, int index, const char *device, const char *path)
{
  BroMakemkvSource *s = g_new0 (BroMakemkvSource, 1);
  s->kind = kind;
  s->index = index;
  s->device = g_strdup (device ? device : "");
  s->path = g_strdup (path ? path : "");
  return s;
}

void
bro_makemkv_source_free (BroMakemkvSource *source)
{
  if (!source)
    return;
  g_free (source->device);
  g_free (source->path);
  g_free (source);
}
