/* bro-queue-orders.c */
#include "bro-queue-orders.h"

BroQueueOrders *
bro_queue_orders_new (void)
{
  BroQueueOrders *o = g_new0 (BroQueueOrders, 1);
  o->acquisition = g_array_new (FALSE, FALSE, sizeof (BroId));
  o->processing = g_array_new (FALSE, FALSE, sizeof (BroId));
  o->maintenance = g_array_new (FALSE, FALSE, sizeof (BroId));
  return o;
}

void
bro_queue_orders_free (BroQueueOrders *orders)
{
  if (!orders)
    return;
  g_array_unref (orders->acquisition);
  g_array_unref (orders->processing);
  g_array_unref (orders->maintenance);
  g_free (orders);
}
