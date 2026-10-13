/* bro-library-record.c */
#include "bro-library-record.h"

BroLibraryRecord *
bro_library_record_new (void)
{
  BroLibraryRecord *x = g_new0 (BroLibraryRecord, 1);
  return x;
}

BroLibraryRecord *
bro_library_record_copy (const BroLibraryRecord *x)
{
  BroLibraryRecord *c = g_new0 (BroLibraryRecord, 1);
  *c = *x;
  c->id = g_strdup (x->id);
  c->name = g_strdup (x->name);
  c->path = g_strdup (x->path);
  c->marker_id = g_strdup (x->marker_id);
  return c;
}

void
bro_library_record_free (BroLibraryRecord *x)
{
  if (!x)
    return;
  g_free (x->id);
  g_free (x->name);
  g_free (x->path);
  g_free (x->marker_id);
  g_free (x);
}
