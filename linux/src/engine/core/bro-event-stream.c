/* bro-event-stream.c */
#include "bro-core-private.h"

struct _BroEventStream {
  GObject parent_instance;
  GMutex lock;
  GCond cond;
  GQueue items; /* of BroStreamItem */
  guint capacity;
  gboolean wanted[BRO_ENGINE_EVENT_TYPE_COUNT];
  gboolean resync_waiting;
  gboolean closed;
};

G_DEFINE_FINAL_TYPE (BroEventStream, bro_event_stream, G_TYPE_OBJECT)

BroEventStream *
_bro_event_stream_new (guint capacity, const BroEngineEventType *types, gsize n_types)
{
  BroEventStream *self = g_object_new (BRO_TYPE_EVENT_STREAM, NULL);
  self->capacity = capacity;
  for (guint t = 0; t < BRO_ENGINE_EVENT_TYPE_COUNT; t++)
    self->wanted[t] = n_types == 0 && !bro_engine_event_type_is_internal (t);
  for (gsize i = 0; i < n_types; i++)
    if ((guint) types[i] < BRO_ENGINE_EVENT_TYPE_COUNT && !bro_engine_event_type_is_internal (types[i]))
      self->wanted[types[i]] = TRUE;
  return self;
}

gboolean
_bro_event_stream_wants (BroEventStream *self, BroEngineEventType type)
{
  return (guint) type < BRO_ENGINE_EVENT_TYPE_COUNT && self->wanted[type];
}

static void
resync_locked (BroEventStream *self)
{
  g_queue_clear_full (&self->items, (GDestroyNotify) bro_stream_item_free);
  g_queue_push_tail (&self->items, bro_stream_item_new (BRO_STREAM_ITEM_RESYNC, NULL));
  self->resync_waiting = TRUE;
  g_cond_broadcast (&self->cond);
}

void
_bro_event_stream_offer (BroEventStream *self, BroEngineEvent *event)
{
  g_mutex_lock (&self->lock);
  if (!self->closed && !self->resync_waiting && _bro_event_stream_wants (self, event->type))
    {
      if (self->items.length >= self->capacity)
        resync_locked (self);
      else
        {
          g_queue_push_tail (&self->items, bro_stream_item_new (BRO_STREAM_ITEM_EVENT, event));
          g_cond_broadcast (&self->cond);
        }
    }
  g_mutex_unlock (&self->lock);
}

void
_bro_event_stream_resync (BroEventStream *self)
{
  g_mutex_lock (&self->lock);
  if (!self->closed)
    resync_locked (self);
  g_mutex_unlock (&self->lock);
}

static BroStreamItem *
take_locked (BroEventStream *self)
{
  BroStreamItem *item = g_queue_pop_head (&self->items);
  if (item && item->kind == BRO_STREAM_ITEM_RESYNC)
    self->resync_waiting = FALSE;
  return item;
}

BroStreamItem *
bro_event_stream_poll (BroEventStream *self)
{
  BroStreamItem *item;
  g_mutex_lock (&self->lock);
  item = take_locked (self);
  g_mutex_unlock (&self->lock);
  return item;
}

BroStreamItem *
bro_event_stream_next (BroEventStream *self)
{
  BroStreamItem *item;
  g_mutex_lock (&self->lock);
  while (!(item = take_locked (self)) && !self->closed)
    g_cond_wait (&self->cond, &self->lock);
  g_mutex_unlock (&self->lock);
  return item;
}

void
bro_event_stream_close (BroEventStream *self)
{
  g_mutex_lock (&self->lock);
  self->closed = TRUE;
  g_queue_clear_full (&self->items, (GDestroyNotify) bro_stream_item_free);
  g_cond_broadcast (&self->cond);
  g_mutex_unlock (&self->lock);
}

gboolean
bro_event_stream_is_closed (BroEventStream *self)
{
  gboolean closed;
  g_mutex_lock (&self->lock);
  closed = self->closed;
  g_mutex_unlock (&self->lock);
  return closed;
}

static void
bro_event_stream_finalize (GObject *object)
{
  BroEventStream *self = BRO_EVENT_STREAM (object);
  g_queue_clear_full (&self->items, (GDestroyNotify) bro_stream_item_free);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);
  G_OBJECT_CLASS (bro_event_stream_parent_class)->finalize (object);
}

static void
bro_event_stream_class_init (BroEventStreamClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_event_stream_finalize;
}

static void
bro_event_stream_init (BroEventStream *self)
{
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  g_queue_init (&self->items);
}
