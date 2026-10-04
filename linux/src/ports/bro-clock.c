/* bro-clock.c */
#include "bro-clock.h"

G_DEFINE_INTERFACE (BroClock, bro_clock, G_TYPE_OBJECT)

static void
bro_clock_default_init (BroClockInterface *iface)
{
}

BroInstant
bro_clock_now (BroClock *self)
{
  g_return_val_if_fail (BRO_IS_CLOCK (self), (BroInstant) { 0 });
  return BRO_CLOCK_GET_IFACE (self)->now (self);
}

BroDuration
bro_clock_monotonic (BroClock *self)
{
  g_return_val_if_fail (BRO_IS_CLOCK (self), (BroDuration) { 0 });
  return BRO_CLOCK_GET_IFACE (self)->monotonic (self);
}

gboolean
bro_clock_sleep (BroClock *self, BroDuration duration, BroCancellationToken *cancel, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CLOCK (self), FALSE);
  return BRO_CLOCK_GET_IFACE (self)->sleep (self, duration, cancel, error);
}

BroTimerHandle *
bro_clock_timer (BroClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy)
{
  g_return_val_if_fail (BRO_IS_CLOCK (self), NULL);
  return BRO_CLOCK_GET_IFACE (self)->timer (self, schedule, handler, data, destroy);
}
