/* bro-subscription.h: BroSubscription: an in-engine subscriber of the BroEventBus. Used on the executor. */
#pragma once

#include "bro-engine-event.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SUBSCRIPTION (bro_subscription_get_type ())
G_DECLARE_FINAL_TYPE (BroSubscription, bro_subscription, BRO, SUBSCRIPTION, GObject)

/* Runs on the executor; @event is borrowed. */
typedef void (*BroEventHandler) (BroEngineEvent *event, gpointer data);

/* No more deliveries, starting with the event being delivered now. Dropping the last reference without cancelling
 * leaves the subscription with the bus. */
void bro_subscription_cancel (BroSubscription *self);

G_END_DECLS
