/* bro-queue-change.c */
#include "bro-queue-change.h"

BroQueueChange *
bro_queue_change_new (BroQueue queue, const GArray *order)
{
  BroQueueChange *c = g_new0 (BroQueueChange, 1);
  c->queue = queue;
  c->order = g_array_new (FALSE, FALSE, sizeof (BroId));
  if (order)
    g_array_append_vals (c->order, order->data, order->len);
  return c;
}

void
bro_queue_change_free (BroQueueChange *change)
{
  if (!change)
    return;
  g_array_unref (change->order);
  g_free (change);
}

BroJsonValue *
bro_queue_change_json (const BroQueueChange *change)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *order = bro_json_value_new_array ();
  for (guint i = 0; i < change->order->len; i++)
    bro_json_value_append (order, bro_json_value_new_string (g_array_index (change->order, BroId, i).value));
  bro_json_value_set (o, "queue", bro_json_value_new_string (bro_queue_to_wire (change->queue)));
  bro_json_value_set (o, "order", order);
  return o;
}
