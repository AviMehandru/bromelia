/* bro-engine-timer.h: BroEngineTimer: a timer of BroEngineClock. Used on the executor. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ENGINE_TIMER (bro_engine_timer_get_type ())
G_DECLARE_FINAL_TYPE (BroEngineTimer, bro_engine_timer, BRO, ENGINE_TIMER, GObject)

/* Stops the timer: a handler the clock already posted doesn't run either. Does nothing the second time, or after a
 * one-off timer fired. */
void bro_engine_timer_cancel (BroEngineTimer *self);

G_END_DECLS
