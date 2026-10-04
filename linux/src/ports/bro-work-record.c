/* bro-work-record.c */
#include "bro-work-record.h"

BroWorkRecord *
bro_work_record_new (void)
{
  BroWorkRecord *x = g_new0 (BroWorkRecord, 1);
  return x;
}

void
bro_work_record_free (BroWorkRecord *x)
{
  if (!x)
    return;
  g_free (x->title);
  g_free (x->imdb_id);
  g_free (x);
}
