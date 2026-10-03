/* bro-decided.c */
#include "bro-decided.h"

void
bro_decided_free (BroDecided *decided)
{
  if (!decided)
    return;
  bro_bro_error_free (decided->error);
  g_free (decided);
}
