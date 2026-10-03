/* bro-decision.c */
#include "bro-decision.h"

void
bro_decision_free (BroDecision *decision)
{
  if (!decision)
    return;
  bro_bro_message_free (decision->reason);
  g_free (decision);
}
