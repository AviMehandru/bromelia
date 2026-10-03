/* bro-outcome-policy.h: settings that change how a job's outcome is decided. None do yet: today's rules are fixed
 * (read errors quarantine, post-processing failures never fail a job). Kept so profiles can add some without
 * changing bro_job_outcome_decide's signature. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int reserved;
} BroOutcomePolicy;

G_END_DECLS
