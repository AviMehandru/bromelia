/* bro-engine-event.h: BroEngineEvent: one event: its seq, when it was published, its type, and its DTO as JSON
 * (shared/schema/events.json). Immutable and reference-counted: the bus, its streams and its subscribers share it. */
#pragma once

#include "bro-engine-event-type.h"
#include "bro-instant.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  gint64 seq;
  BroInstant at;
  BroEngineEventType type;
  BroJsonValue *data;
} BroEngineEvent;

/* Takes a reference to @data. */
BroEngineEvent *bro_engine_event_new (gint64 seq, BroInstant at, BroEngineEventType type, BroJsonValue *data);
BroEngineEvent *bro_engine_event_ref (BroEngineEvent *event);
void bro_engine_event_unref (BroEngineEvent *event);

/* {seq, at, type, data}: the envelope clients get. Free with bro_json_value_unref. */
BroJsonValue *bro_engine_event_json (const BroEngineEvent *event);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEngineEvent, bro_engine_event_unref)

G_END_DECLS
