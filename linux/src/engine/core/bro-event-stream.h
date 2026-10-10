/* bro-event-stream.h: BroEventStream: one reader's bounded queue of client-visible events (the server's SSE
 * connection). The bus fills it on the executor; the reader takes from it on any thread. When an event doesn't fit,
 * everything queued becomes one resync item, and while a resync waits, new events are dropped: the snapshot the
 * reader sends next covers them. */
#pragma once

#include "bro-stream-item.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_EVENT_STREAM (bro_event_stream_get_type ())
G_DECLARE_FINAL_TYPE (BroEventStream, bro_event_stream, BRO, EVENT_STREAM, GObject)

/* Waits for the next item; NULL once the stream is closed. Free the item with bro_stream_item_free. */
BroStreamItem *bro_event_stream_next (BroEventStream *self);

/* The next item if one is queued, else NULL; never waits. */
BroStreamItem *bro_event_stream_poll (BroEventStream *self);

/* Ends the stream, from any thread: what was queued is dropped, the bus forgets it, next returns NULL. */
void bro_event_stream_close (BroEventStream *self);

gboolean bro_event_stream_is_closed (BroEventStream *self);

G_END_DECLS
