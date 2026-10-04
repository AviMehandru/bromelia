/* bro-timer-schedule.h: when a timer fires: once at an instant, or every interval. */
#pragma once

#include "bro-duration.h"
#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_TIMER_SCHEDULE_AT, /* instant */
  BRO_TIMER_SCHEDULE_EVERY, /* interval */
} BroTimerScheduleKind;

typedef struct {
  BroTimerScheduleKind kind;
  BroInstant instant;
  BroDuration interval;
} BroTimerSchedule;

/* Everything zero. */
BroTimerSchedule *bro_timer_schedule_new (void);
void bro_timer_schedule_free (BroTimerSchedule *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroTimerSchedule, bro_timer_schedule_free)

G_END_DECLS
