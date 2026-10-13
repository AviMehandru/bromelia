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

BroUnitRecord *
bro_unit_record_copy (const BroUnitRecord *x)
{
  BroUnitRecord *c = g_new0 (BroUnitRecord, 1);
  *c = *x;
  c->library_id = g_strdup (x->library_id);
  c->path = g_strdup (x->path);
  c->record_file = g_strdup (x->record_file);
  c->name = g_strdup (x->name);
  c->format = g_strdup (x->format);
  c->format_code = g_strdup (x->format_code);
  c->fingerprint = g_strdup (x->fingerprint);
  c->label = g_strdup (x->label);
  c->makemkv_version = g_strdup (x->makemkv_version);
  return c;
}
