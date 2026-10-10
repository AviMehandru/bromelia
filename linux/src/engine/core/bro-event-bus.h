/* bro-event-bus.h: BroEventBus: engine events (plan §11.2), used on the executor. Every event gets the next seq;
 * in-engine subscribers get events in seq order (one published by a handler waits until the current one has reached
 * every subscriber); the last @retain events are kept so a reconnecting stream can catch up; streams carry
 * client-visible events only. shared/fixtures/engine/event-bus.cases.json. */
#pragma once

#include "bro-clock.h"
#include "bro-engine-event.h"
#include "bro-event-stream.h"
#include "bro-subscription.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_EVENT_BUS (bro_event_bus_get_type ())
G_DECLARE_FINAL_TYPE (BroEventBus, bro_event_bus, BRO, EVENT_BUS, GObject)

/* @clock gives each event its time. @first_seq is the first event's seq: a host starts each engine above the last
 * one's, so a stream asking for an earlier engine's events gets a resync instead of this engine's events under the
 * same numbers. @retain: how many recent events a stream can catch up on. @stream_capacity: how many items a stream
 * holds before its reader is sent a resync. */
BroEventBus *bro_event_bus_new (BroClock *clock, gint64 first_seq, guint retain, guint stream_capacity);

/* The next seq and the clock's now; takes a reference to @data. Returns the event (unref it). */
BroEngineEvent *bro_event_bus_publish (BroEventBus *self, BroEngineEventType type, BroJsonValue *data);

/* @handler (@data) gets each event of @types (every type, internal ones too, when @n_types is 0) published from now
 * on; @destroy frees @data when the subscription goes. Unref the subscription when done with it. */
BroSubscription *bro_event_bus_subscribe (BroEventBus *self, const BroEngineEventType *types, gsize n_types,
                                          BroEventHandler handler, gpointer data, GDestroyNotify destroy);

/* A stream of the client-visible events of @types (all when @n_types is 0). With @has_after it first replays the
 * retained events after @after; it starts with a resync instead when @after is beyond the last seq, when events
 * after it are no longer retained, or when there are more of them than a stream holds. Unref it when done. */
BroEventStream *bro_event_bus_stream (BroEventBus *self, gboolean has_after, gint64 after, const BroEngineEventType *types,
                                      gsize n_types);

/* The last seq published (first_seq - 1 before the first). */
gint64 bro_event_bus_last_seq (BroEventBus *self);

G_END_DECLS
