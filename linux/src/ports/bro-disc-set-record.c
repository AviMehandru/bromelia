/* bro-disc-set-record.c */
#include "bro-disc-set-record.h"

BroDiscSetRecord *
bro_disc_set_record_new (void)
{
  BroDiscSetRecord *x = g_new0 (BroDiscSetRecord, 1);
  return x;
}

void
bro_disc_set_record_free (BroDiscSetRecord *x)
{
  if (!x)
    return;
  g_free (x->label_title);
  g_free (x->description);
  g_free (x);
}
