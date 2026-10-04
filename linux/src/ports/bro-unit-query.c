/* bro-unit-query.c */
#include "bro-unit-query.h"

BroUnitQuery *
bro_unit_query_new (void)
{
  BroUnitQuery *x = g_new0 (BroUnitQuery, 1);
  return x;
}

void
bro_unit_query_free (BroUnitQuery *x)
{
  if (!x)
    return;
  g_free (x->library_id);
  g_free (x->text);
  g_free (x);
}
