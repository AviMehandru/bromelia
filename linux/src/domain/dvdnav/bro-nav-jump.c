/* bro-nav-jump.c */
#include "bro-nav-jump.h"

BroNavJump *
bro_nav_jump_new (int title, int chapter, const char *how)
{
  BroNavJump *j = g_new0 (BroNavJump, 1);
  j->title = title;
  j->chapter = chapter;
  j->how = g_strdup (how);
  return j;
}

void
bro_nav_jump_free (BroNavJump *jump)
{
  if (!jump)
    return;
  g_free (jump->how);
  g_free (jump);
}
