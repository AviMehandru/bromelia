/* bro-timer-schedule.c */
#include "bro-timer-schedule.h"

BroTimerSchedule *
bro_timer_schedule_new (void)
{
  BroTimerSchedule *x = g_new0 (BroTimerSchedule, 1);
  return x;
}

void
bro_timer_schedule_free (BroTimerSchedule *x)
{
  if (!x)
    return;
  g_free (x);
}
