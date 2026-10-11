/* bro-wait.c */
#include "bro-wait.h"

BroWait *
bro_wait_new (BroWaitKind kind, const BroBroError *reason)
{
  BroWait *w = g_new0 (BroWait, 1);
  w->kind = kind;
  w->reason = reason ? bro_bro_error_copy (reason) : NULL;
  return w;
}

void
bro_wait_free (BroWait *wait)
{
  if (!wait)
    return;
  bro_bro_error_free (wait->reason);
  g_free (wait);
}
