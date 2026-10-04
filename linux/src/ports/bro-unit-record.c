/* bro-unit-record.c */
#include "bro-unit-record.h"

BroUnitRecord *
bro_unit_record_new (void)
{
  BroUnitRecord *x = g_new0 (BroUnitRecord, 1);
  return x;
}

void
bro_unit_record_free (BroUnitRecord *x)
{
  if (!x)
    return;
  g_free (x->library_id);
  g_free (x->path);
  g_free (x->record_file);
  g_free (x->name);
  g_free (x->format);
  g_free (x->format_code);
  g_free (x->fingerprint);
  g_free (x->label);
  g_free (x->makemkv_version);
  g_free (x);
}
