/* bro-event-feed.h: BroEventFeed: what bro_engine_api_events gives (decision 168): the snapshot event first when it was
 * opened without a replayable after, then the deltas; after a gap (the stream's resync) a new snapshot instead of the
 * missed deltas; jobLog events only for the jobs asked for. Read from any thread, one reader at a time; stop closes it.
 * Each next waits for its event on a thread of its own, so a feed nobody reads adds nothing to the executor's
 * activity. */
#pragma once

#include "bro-engine-event.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_EVENT_FEED (bro_event_feed_get_type ())
G_DECLARE_FINAL_TYPE (BroEventFeed, bro_event_feed, BRO, EVENT_FEED, GObject)

/* The next event (borrowed), or NULL once the feed is closed. On the executor; once the executor has stopped, on the
 * thread that found the feed closed. */
typedef void (*BroEventFeedDone) (const BroEngineEvent *event, gpointer data);

/* The next event (the snapshot first); NULL once closed. Never fails: a snapshot that can't be built after a gap (the
 * engine stopped) closes the feed. @destroy frees @data after @done. */
void bro_event_feed_next (BroEventFeed *self, BroEventFeedDone done, gpointer data, GDestroyNotify destroy);

/* Ends the feed, from any thread: next gives NULL from now on. Returns once no next waits for an event any more. */
void bro_event_feed_close (BroEventFeed *self);

G_END_DECLS
