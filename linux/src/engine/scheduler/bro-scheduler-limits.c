/* bro-scheduler-limits.c */
#include "bro-scheduler-limits.h"

BroSchedulerLimits *
bro_scheduler_limits_new (void)
{
  BroSchedulerLimits *l = g_new0 (BroSchedulerLimits, 1);
  l->processing = 1;
  l->cpu = 2;
  l->io_per_volume = 2;
  return l;
}

void
bro_scheduler_limits_free (BroSchedulerLimits *limits)
{
  g_free (limits);
}
