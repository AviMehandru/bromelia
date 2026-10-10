/* bro-subscription.c */
#include "bro-core-private.h"

struct _BroSubscription {
  GObject parent_instance;
  gpointer bus; /* not a reference; NULL once the bus is gone */
  gboolean wanted[BRO_ENGINE_EVENT_TYPE_COUNT];
  gint64 from_seq;
  BroEventHandler handler;
  gpointer data;
  GDestroyNotify destroy;
  gboolean cancelled;
};

G_DEFINE_FINAL_TYPE (BroSubscription, bro_subscription, G_TYPE_OBJECT)

BroSubscription *
_bro_subscription_new (gpointer bus, const BroEngineEventType *types, gsize n_types, gint64 from_seq,
                       BroEventHandler handler, gpointer data, GDestroyNotify destroy)
{
  BroSubscription *self = g_object_new (BRO_TYPE_SUBSCRIPTION, NULL);
  self->bus = bus;
  for (guint t = 0; t < BRO_ENGINE_EVENT_TYPE_COUNT; t++)
    self->wanted[t] = n_types == 0;
  for (gsize i = 0; i < n_types; i++)
    if ((guint) types[i] < BRO_ENGINE_EVENT_TYPE_COUNT)
      self->wanted[types[i]] = TRUE;
  self->from_seq = from_seq;
  self->handler = handler;
  self->data = data;
  self->destroy = destroy;
  return self;
}

gboolean
_bro_subscription_wants (BroSubscription *self, const BroEngineEvent *event)
{
  return !self->cancelled && event->seq >= self->from_seq && (guint) event->type < BRO_ENGINE_EVENT_TYPE_COUNT
         && self->wanted[event->type];
}

void
_bro_subscription_deliver (BroSubscription *self, BroEngineEvent *event)
{
  self->handler (event, self->data);
}

void
_bro_subscription_detach (BroSubscription *self)
{
  self->bus = NULL;
}

void
bro_subscription_cancel (BroSubscription *self)
{
  if (self->cancelled)
    return;
  self->cancelled = TRUE;
  if (self->bus)
    _bro_event_bus_remove (self->bus, self);
}

static void
bro_subscription_finalize (GObject *object)
{
  BroSubscription *self = BRO_SUBSCRIPTION (object);
  if (self->destroy)
    self->destroy (self->data);
  G_OBJECT_CLASS (bro_subscription_parent_class)->finalize (object);
}

static void
bro_subscription_class_init (BroSubscriptionClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_subscription_finalize;
}

static void
bro_subscription_init (BroSubscription *self)
{
}
