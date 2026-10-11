/* bro-queue-orders.h: the jobs of each queue, in order. */
#pragma once

#include "bro-id.h"

G_BEGIN_DECLS

typedef struct {
  GArray *acquisition; /* of BroId */
  GArray *processing;
  GArray *maintenance;
} BroQueueOrders;

/* Three empty lists. */
BroQueueOrders *bro_queue_orders_new (void);
void bro_queue_orders_free (BroQueueOrders *orders);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroQueueOrders, bro_queue_orders_free)

G_END_DECLS
