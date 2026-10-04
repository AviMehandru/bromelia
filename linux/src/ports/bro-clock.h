/* bro-clock.h: BroClock: Time. */
#pragma once

#include "bro-bro-error.h"
#include "bro-cancellation-token.h"
#include "bro-duration.h"
#include "bro-instant.h"
#include "bro-timer-handle.h"
#include "bro-timer-schedule.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CLOCK (bro_clock_get_type ())
G_DECLARE_INTERFACE (BroClock, bro_clock, BRO, CLOCK, GObject)

typedef void (*BroTimerFunc) (gpointer data);

struct _BroClockInterface {
  GTypeInterface parent_iface;

  BroInstant (*now) (BroClock *self);
  BroDuration (*monotonic) (BroClock *self);
  gboolean (*sleep) (BroClock *self, BroDuration duration, BroCancellationToken *cancel, BroBroError **error);
  BroTimerHandle *(*timer) (BroClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy);
};

BroInstant bro_clock_now (BroClock *self);

/* Time since an arbitrary start; never goes back. */
BroDuration bro_clock_monotonic (BroClock *self);

/* Fails with job.cancelled when cancelled first. FALSE and @error set on failure. */
gboolean bro_clock_sleep (BroClock *self, BroDuration duration, BroCancellationToken *cancel, BroBroError **error);

BroTimerHandle *bro_clock_timer (BroClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy);

G_END_DECLS
