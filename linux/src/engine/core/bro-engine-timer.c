/* bro-engine-timer.c */
#include "bro-core-private.h"

struct _BroEngineTimer {
  GObject parent_instance;
  BroTimerFunc handler;
  gpointer data;
  GDestroyNotify destroy;
  gboolean once;
  gboolean ended;
  BroTimerHandle *handle; /* until the timer ends: the clock's timer holds a reference to this one meanwhile */
};

G_DEFINE_FINAL_TYPE (BroEngineTimer, bro_engine_timer, G_TYPE_OBJECT)

BroEngineTimer *
_bro_engine_timer_new (BroTimerFunc handler, gpointer data, GDestroyNotify destroy, gboolean once)
{
  BroEngineTimer *self = g_object_new (BRO_TYPE_ENGINE_TIMER, NULL);
  self->handler = handler;
  self->data = data;
  self->destroy = destroy;
  self->once = once;
  return self;
}

void
_bro_engine_timer_set_handle (BroEngineTimer *self, BroTimerHandle *handle)
{
  if (self->ended)
    {
      bro_timer_handle_cancel (handle);
      g_object_unref (handle);
      return;
    }
  self->handle = handle;
}

/* Lets go of the clock's timer, which lets go of this one. */
static void
end (BroEngineTimer *self)
{
  BroTimerHandle *handle = g_steal_pointer (&self->handle);
  self->ended = TRUE;
  if (handle)
    {
      bro_timer_handle_cancel (handle);
      g_object_unref (handle);
    }
}

void
bro_engine_timer_cancel (BroEngineTimer *self)
{
  if (!self->ended)
    end (self);
}

void
_bro_engine_timer_fire (BroEngineTimer *self)
{
  if (self->ended)
    return;
  if (self->once)
    end (self);
  self->handler (self->data);
}

static void
bro_engine_timer_finalize (GObject *object)
{
  BroEngineTimer *self = BRO_ENGINE_TIMER (object);
  g_clear_object (&self->handle);
  if (self->destroy)
    self->destroy (self->data);
  G_OBJECT_CLASS (bro_engine_timer_parent_class)->finalize (object);
}

static void
bro_engine_timer_class_init (BroEngineTimerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_engine_timer_finalize;
}

static void
bro_engine_timer_init (BroEngineTimer *self)
{
}
