/* bro-engine-clock.c */
#include "bro-engine-clock.h"

#include "bro-core-private.h"

struct _BroEngineClock {
  GObject parent_instance;
  BroClock *clock;
  BroEngineExecutor *executor;
};

G_DEFINE_FINAL_TYPE (BroEngineClock, bro_engine_clock, G_TYPE_OBJECT)

BroEngineClock *
bro_engine_clock_new (BroClock *clock, BroEngineExecutor *executor)
{
  BroEngineClock *self = g_object_new (BRO_TYPE_ENGINE_CLOCK, NULL);
  self->clock = g_object_ref (clock);
  self->executor = g_object_ref (executor);
  return self;
}

BroInstant
bro_engine_clock_now (BroEngineClock *self)
{
  return bro_clock_now (self->clock);
}

BroDuration
bro_engine_clock_monotonic (BroEngineClock *self)
{
  return bro_clock_monotonic (self->clock);
}

/* What the clock's timer holds. */
typedef struct {
  BroEngineExecutor *executor;
  BroEngineTimer *timer;
} Firing;

static void
firing_free (gpointer data)
{
  Firing *f = data;
  g_object_unref (f->executor);
  g_object_unref (f->timer);
  g_free (f);
}

static void
fire_on_executor (gpointer data)
{
  _bro_engine_timer_fire (data);
}

/* On the clock's thread. */
static void
fired (gpointer data)
{
  Firing *f = data;
  bro_engine_executor_post (f->executor, fire_on_executor, g_object_ref (f->timer), g_object_unref);
}

BroEngineTimer *
bro_engine_clock_timer (BroEngineClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data,
                        GDestroyNotify destroy)
{
  BroEngineTimer *timer = _bro_engine_timer_new (handler, data, destroy, schedule->kind == BRO_TIMER_SCHEDULE_AT);
  Firing *f = g_new0 (Firing, 1);
  f->executor = g_object_ref (self->executor);
  f->timer = g_object_ref (timer);
  _bro_engine_timer_set_handle (timer, bro_clock_timer (self->clock, schedule, fired, f, firing_free));
  return timer;
}

static void
bro_engine_clock_finalize (GObject *object)
{
  BroEngineClock *self = BRO_ENGINE_CLOCK (object);
  g_object_unref (self->clock);
  g_object_unref (self->executor);
  G_OBJECT_CLASS (bro_engine_clock_parent_class)->finalize (object);
}

static void
bro_engine_clock_class_init (BroEngineClockClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_engine_clock_finalize;
}

static void
bro_engine_clock_init (BroEngineClock *self)
{
}
