/* bro-scheduler-limits.h: the configuration's limits, resolved (plan §21): acquisitions 0 = no cap; cpu is a
 * count (the caller turns the configuration's 0 into cores / 2: the engine can't ask the system). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int acquisitions;
  int processing;
  int cpu;
  int io_per_volume;
} BroSchedulerLimits;

/* 0, 1, 2, 2. */
BroSchedulerLimits *bro_scheduler_limits_new (void);
void bro_scheduler_limits_free (BroSchedulerLimits *limits);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSchedulerLimits, bro_scheduler_limits_free)

G_END_DECLS
