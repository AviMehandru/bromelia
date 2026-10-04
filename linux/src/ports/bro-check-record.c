/* bro-check-record.c */
#include "bro-check-record.h"

BroCheckRecord *
bro_check_record_new (void)
{
  BroCheckRecord *x = g_new0 (BroCheckRecord, 1);
  x->changed = g_new0 (char *, 1);
  x->unreadable = g_new0 (char *, 1);
  x->missing = g_new0 (char *, 1);
  x->unlisted = g_new0 (char *, 1);
  return x;
}

void
bro_check_record_free (BroCheckRecord *x)
{
  if (!x)
    return;
  g_free (x->folder);
  g_strfreev (x->changed);
  g_strfreev (x->unreadable);
  g_strfreev (x->missing);
  g_strfreev (x->unlisted);
  g_clear_pointer (&x->error, bro_bro_message_free);
  g_free (x);
}
