/* bro-api-private.h: what LocalEngine and EventFeed share (internal in Swift and C# too). */
#pragma once

#include "bro-engine-executor.h"
#include "bro-event-feed.h"
#include "bro-event-stream.h"
#include "bro-id.h"

G_BEGIN_DECLS

/* A snapshot event (taken), or NULL when the engine stopped while it was asked for. On the executor. */
typedef void (*BroSnapshotReady) (BroEngineEvent *event, gpointer data);
/* Builds a new snapshot event on the executor, then calls @ready there. */
typedef void (*BroSnapshotFunc) (GObject *owner, BroSnapshotReady ready, gpointer data);

/* A feed over @stream (taken) with the jobLog events of @job_log only; @first (taken, nullable) is the snapshot event
 * to give first. After a gap @snapshot asks @owner (held weakly: a feed doesn't keep the engine) for a new one. */
BroEventFeed *_bro_event_feed_new (BroEngineExecutor *executor, BroEventStream *stream, const BroId *job_log, gsize n_job_log,
                                   BroEngineEvent *first, BroSnapshotFunc snapshot, GObject *owner);
gboolean _bro_event_feed_is_closed (BroEventFeed *self);

G_END_DECLS
