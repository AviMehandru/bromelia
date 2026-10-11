/* bro-queue-change.h: the data of a queueChanged event (shared/schema/events.json). */
#pragma once

#include "bro-id.h"
#include "bro-json-value.h"
#include "bro-queue.h"

G_BEGIN_DECLS

typedef struct {
  BroQueue queue;
  GArray *order; /* of BroId */
} BroQueueChange;

/* Copies @order (may be NULL: empty). */
BroQueueChange *bro_queue_change_new (BroQueue queue, const GArray *order);
void bro_queue_change_free (BroQueueChange *change);

/* {queue, order}. Free with bro_json_value_unref. */
BroJsonValue *bro_queue_change_json (const BroQueueChange *change);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroQueueChange, bro_queue_change_free)

G_END_DECLS
