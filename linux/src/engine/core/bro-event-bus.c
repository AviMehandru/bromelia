/* bro-event-bus.c */
#include "bro-event-bus.h"

#include "bro-core-private.h"

struct _BroEventBus {
  GObject parent_instance;
  BroClock *clock;
  guint retain;
  guint stream_capacity;
  gint64 last_seq;
  GQueue retained;          /* of BroEngineEvent */
  GPtrArray *subscriptions; /* of BroSubscription, references */
  GPtrArray *streams;       /* of BroEventStream, references */
  GQueue undelivered;       /* of BroEngineEvent */
  gboolean delivering;
};

G_DEFINE_FINAL_TYPE (BroEventBus, bro_event_bus, G_TYPE_OBJECT)

BroEventBus *
bro_event_bus_new (BroClock *clock, gint64 first_seq, guint retain, guint stream_capacity)
{
  BroEventBus *self = g_object_new (BRO_TYPE_EVENT_BUS, NULL);
  self->clock = g_object_ref (clock);
  self->retain = retain;
  self->stream_capacity = stream_capacity;
  self->last_seq = first_seq - 1;
  return self;
}

gint64
bro_event_bus_last_seq (BroEventBus *self)
{
  return self->last_seq;
}

static void
deliver_all (BroEventBus *self)
{
  BroEngineEvent *next;
  while ((next = g_queue_pop_head (&self->undelivered)))
    {
      /* A copy, with references: handlers may subscribe or cancel. */
      g_autoptr (GPtrArray) subs = g_ptr_array_new_full (self->subscriptions->len, g_object_unref);
      for (guint i = 0; i < self->subscriptions->len; i++)
        g_ptr_array_add (subs, g_object_ref (g_ptr_array_index (self->subscriptions, i)));
      for (guint i = 0; i < subs->len; i++)
        {
          BroSubscription *sub = g_ptr_array_index (subs, i);
          if (_bro_subscription_wants (sub, next))
            _bro_subscription_deliver (sub, next);
        }
      bro_engine_event_unref (next);
    }
}

BroEngineEvent *
bro_event_bus_publish (BroEventBus *self, BroEngineEventType type, BroJsonValue *data)
{
  BroEngineEvent *e = bro_engine_event_new (self->last_seq + 1, bro_clock_now (self->clock), type, data);
  self->last_seq = e->seq;
  g_queue_push_tail (&self->retained, bro_engine_event_ref (e));
  while (self->retained.length > self->retain)
    bro_engine_event_unref (g_queue_pop_head (&self->retained));
  if (!bro_engine_event_type_is_internal (type))
    {
      for (guint i = self->streams->len; i > 0; i--)
        if (bro_event_stream_is_closed (g_ptr_array_index (self->streams, i - 1)))
          g_ptr_array_remove_index (self->streams, i - 1);
      for (guint i = 0; i < self->streams->len; i++)
        _bro_event_stream_offer (g_ptr_array_index (self->streams, i), e);
    }
  g_queue_push_tail (&self->undelivered, bro_engine_event_ref (e));
  if (!self->delivering)
    {
      self->delivering = TRUE;
      deliver_all (self);
      self->delivering = FALSE;
    }
  return e;
}

BroSubscription *
bro_event_bus_subscribe (BroEventBus *self, const BroEngineEventType *types, gsize n_types, BroEventHandler handler,
                         gpointer data, GDestroyNotify destroy)
{
  BroSubscription *sub = _bro_subscription_new (self, types, n_types, self->last_seq + 1, handler, data, destroy);
  g_ptr_array_add (self->subscriptions, g_object_ref (sub));
  return sub;
}

void
_bro_event_bus_remove (gpointer bus, BroSubscription *sub)
{
  BroEventBus *self = bus;
  g_ptr_array_remove (self->subscriptions, sub);
}

BroEventStream *
bro_event_bus_stream (BroEventBus *self, gboolean has_after, gint64 after, const BroEngineEventType *types, gsize n_types)
{
  BroEventStream *stream = _bro_event_stream_new (self->stream_capacity, types, n_types);
  if (has_after && after != self->last_seq)
    {
      BroEngineEvent *first = g_queue_peek_head (&self->retained);
      gint64 oldest = first ? first->seq : self->last_seq + 1;
      guint replay = 0;
      for (GList *l = self->retained.head; l; l = l->next)
        {
          BroEngineEvent *e = l->data;
          if (e->seq > after && _bro_event_stream_wants (stream, e->type))
            replay++;
        }
      if (after > self->last_seq || after < oldest - 1 || replay > self->stream_capacity)
        _bro_event_stream_resync (stream);
      else
        for (GList *l = self->retained.head; l; l = l->next)
          {
            BroEngineEvent *e = l->data;
            if (e->seq > after)
              _bro_event_stream_offer (stream, e);
          }
    }
  g_ptr_array_add (self->streams, g_object_ref (stream));
  return stream;
}

static void
bro_event_bus_finalize (GObject *object)
{
  BroEventBus *self = BRO_EVENT_BUS (object);
  for (guint i = 0; i < self->subscriptions->len; i++)
    _bro_subscription_detach (g_ptr_array_index (self->subscriptions, i));
  g_ptr_array_unref (self->subscriptions);
  for (guint i = 0; i < self->streams->len; i++)
    bro_event_stream_close (g_ptr_array_index (self->streams, i));
  g_ptr_array_unref (self->streams);
  g_queue_clear_full (&self->retained, (GDestroyNotify) bro_engine_event_unref);
  g_queue_clear_full (&self->undelivered, (GDestroyNotify) bro_engine_event_unref);
  g_object_unref (self->clock);
  G_OBJECT_CLASS (bro_event_bus_parent_class)->finalize (object);
}

static void
bro_event_bus_class_init (BroEventBusClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_event_bus_finalize;
}

static void
bro_event_bus_init (BroEventBus *self)
{
  g_queue_init (&self->retained);
  g_queue_init (&self->undelivered);
  self->subscriptions = g_ptr_array_new_with_free_func (g_object_unref);
  self->streams = g_ptr_array_new_with_free_func (g_object_unref);
}
