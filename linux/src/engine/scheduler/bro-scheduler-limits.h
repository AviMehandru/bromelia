/* bro-scheduler-limits.h: the configuration's limits, resolved (plan §21): acquisitions 0 = no cap; cpu is a
 * count (the caller turns the configuration's 0 into cores / 2: the engine can't ask the system). */
#pragma once

#include "bro-config.h"
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

/* The configuration's limits (config-3 Limits, its defaults where a member is missing); cpu 0 = max(1, @cores / 2).
 * LocalEngine applies it after the load and on every configChanged. */
BroSchedulerLimits *bro_scheduler_limits_of (const BroConfig *config, int cores);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSchedulerLimits, bro_scheduler_limits_free)

G_END_DECLS
