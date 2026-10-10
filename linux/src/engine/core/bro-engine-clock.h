/* bro-engine-clock.h: BroEngineClock: the Clock port as engine code sees it: now and monotonic as the clock says; a
 * timer's handler runs on the executor, whatever thread the clock fires it on, and never after the timer was
 * cancelled. */
#pragma once

#include "bro-clock.h"
#include "bro-engine-executor.h"
#include "bro-engine-timer.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ENGINE_CLOCK (bro_engine_clock_get_type ())
G_DECLARE_FINAL_TYPE (BroEngineClock, bro_engine_clock, BRO, ENGINE_CLOCK, GObject)

BroEngineClock *bro_engine_clock_new (BroClock *clock, BroEngineExecutor *executor);

BroInstant bro_engine_clock_now (BroEngineClock *self);

BroDuration bro_engine_clock_monotonic (BroEngineClock *self);

/* Call on the executor; @handler (@data) runs there. Like the clock's own timers, it runs until it has fired (at) or
 * is cancelled, whether or not its reference is kept. @destroy frees @data once the timer has ended and is
 * unreferenced. */
BroEngineTimer *bro_engine_clock_timer (BroEngineClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler,
                                        gpointer data, GDestroyNotify destroy);

G_END_DECLS
