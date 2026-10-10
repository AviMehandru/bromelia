/* bro-stream-item.h: what a BroEventStream yields: an event, or resync (events were lost: send a snapshot). */
#pragma once

#include "bro-engine-event.h"

G_BEGIN_DECLS

typedef enum {
  BRO_STREAM_ITEM_EVENT,  /* event */
  BRO_STREAM_ITEM_RESYNC,
} BroStreamItemKind;

typedef struct {
  BroStreamItemKind kind;
  BroEngineEvent *event; /* BRO_STREAM_ITEM_EVENT only; owned */
} BroStreamItem;

/* Takes a reference to @event (NULL for resync). */
BroStreamItem *bro_stream_item_new (BroStreamItemKind kind, BroEngineEvent *event);
void bro_stream_item_free (BroStreamItem *item);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStreamItem, bro_stream_item_free)

G_END_DECLS
