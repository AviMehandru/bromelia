/* bro-system-clock.h: BroSystemClock: the real clock: wall time, a monotonic clock, cancellable sleeps and timers.
 * A timer runs on a thread of its own and lives until it has fired (at) or is cancelled, whether or not its handle
 * is kept. */
#pragma once

#include "bro-clock.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SYSTEM_CLOCK (bro_system_clock_get_type ())
G_DECLARE_FINAL_TYPE (BroSystemClock, bro_system_clock, BRO, SYSTEM_CLOCK, GObject)

BroSystemClock *bro_system_clock_new (void);

BroInstant bro_system_clock_now (BroSystemClock *self);

/* g_get_monotonic_time. */
BroDuration bro_system_clock_monotonic (BroSystemClock *self);

/* FALSE and @error set (job.cancelled) when @cancel fires first. */
gboolean bro_system_clock_sleep (BroSystemClock *self, BroDuration duration, BroCancellationToken *cancel, BroBroError **error);

/* @handler (@data) runs on the timer's thread; @destroy frees @data when the timer ends. */
BroTimerHandle *bro_system_clock_timer (BroSystemClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler,
                                        gpointer data, GDestroyNotify destroy);

G_END_DECLS
