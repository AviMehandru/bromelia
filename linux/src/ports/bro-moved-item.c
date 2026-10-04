/* bro-moved-item.c */
#include "bro-moved-item.h"

BroMovedItem *
bro_moved_item_new (void)
{
  BroMovedItem *x = g_new0 (BroMovedItem, 1);
  return x;
}

void
bro_moved_item_free (BroMovedItem *x)
{
  if (!x)
    return;
  g_free (x->from);
  g_free (x->to);
  g_free (x);
}
