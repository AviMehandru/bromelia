/* bro-core-private.h: what the core's files share (internal in Swift and C# too). */
#pragma once

#include "bro-clock.h"
#include "bro-engine-timer.h"
#include "bro-event-stream.h"
#include "bro-subscription.h"
#include "bro-timer-handle.h"

G_BEGIN_DECLS

/* A stream of at most @capacity items of the client-visible @types (all when @n_types is 0). */
BroEventStream *_bro_event_stream_new (guint capacity, const BroEngineEventType *types, gsize n_types);
gboolean _bro_event_stream_wants (BroEventStream *self, BroEngineEventType type);
/* Queues @event (when wanted and there is room; otherwise a resync). */
void _bro_event_stream_offer (BroEventStream *self, BroEngineEvent *event);
/* Replaces what is queued with one resync item. */
void _bro_event_stream_resync (BroEventStream *self);

/* A subscription of @bus (not a reference: the bus outlives its subscriptions' deliveries) to the events from
 * @from_seq on. */
BroSubscription *_bro_subscription_new (gpointer bus, const BroEngineEventType *types, gsize n_types, gint64 from_seq,
                                        BroEventHandler handler, gpointer data, GDestroyNotify destroy);
gboolean _bro_subscription_wants (BroSubscription *self, const BroEngineEvent *event);
void _bro_subscription_deliver (BroSubscription *self, BroEngineEvent *event);
/* The bus forgets @sub (it called bro_subscription_cancel). */
void _bro_event_bus_remove (gpointer bus, BroSubscription *sub);
/* The bus is gone: cancel no longer tells it. */
void _bro_subscription_detach (BroSubscription *self);

/* A timer running @handler once (@once) or until cancelled. */
BroEngineTimer *_bro_engine_timer_new (BroTimerFunc handler, gpointer data, GDestroyNotify destroy, gboolean once);
void _bro_engine_timer_set_handle (BroEngineTimer *self, BroTimerHandle *handle);
/* Runs the handler unless cancelled (on the executor). */
void _bro_engine_timer_fire (BroEngineTimer *self);

G_END_DECLS
