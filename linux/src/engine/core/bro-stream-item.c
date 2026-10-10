/* bro-stream-item.c */
#include "bro-stream-item.h"

BroStreamItem *
bro_stream_item_new (BroStreamItemKind kind, BroEngineEvent *event)
{
  BroStreamItem *item = g_new0 (BroStreamItem, 1);
  item->kind = kind;
  item->event = event ? bro_engine_event_ref (event) : NULL;
  return item;
}

void
bro_stream_item_free (BroStreamItem *item)
{
  if (!item)
    return;
  bro_engine_event_unref (item->event);
  g_free (item);
}
