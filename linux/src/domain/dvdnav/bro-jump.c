/* bro-jump.c */
#include "bro-jump.h"

void
bro_jump_free (BroJump *jump)
{
  if (!jump)
    return;
  g_free (jump->condition);
  g_free (jump);
}
