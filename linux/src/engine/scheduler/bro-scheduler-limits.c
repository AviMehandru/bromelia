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

BroSchedulerLimits *
bro_scheduler_limits_of (const BroConfig *config, int cores)
{
  BroJsonValue *limits = bro_json_value_member (config->document, "limits");
  BroSchedulerLimits *l = g_new0 (BroSchedulerLimits, 1);
  int cpu = (int) bro_json_value_get_integer (bro_json_value_member (limits, "cpu"), 0);
  l->acquisitions = (int) bro_json_value_get_integer (bro_json_value_member (limits, "acquisitions"), 0);
  l->processing = (int) bro_json_value_get_integer (bro_json_value_member (limits, "processing"), 1);
  l->cpu = cpu > 0 ? cpu : MAX (1, cores / 2);
  l->io_per_volume = (int) bro_json_value_get_integer (bro_json_value_member (limits, "ioPerVolume"), 2);
  return l;
}
