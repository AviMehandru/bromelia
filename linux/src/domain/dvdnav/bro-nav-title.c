/* bro-nav-title.c */
#include "bro-nav-title.h"

BroNavTitle *
bro_nav_title_new (int number)
{
  BroNavTitle *t = g_new0 (BroNavTitle, 1);
  t->number = number;
  t->chapters = g_array_new (FALSE, FALSE, sizeof (double));
  t->chapter_vobs = g_array_new (FALSE, FALSE, sizeof (int));
  return t;
}

void
bro_nav_title_free (BroNavTitle *title)
{
  if (!title)
    return;
  g_array_unref (title->chapters);
  g_array_unref (title->chapter_vobs);
  g_free (title);
}
