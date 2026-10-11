/* bro-job-totals.h: a job's counts (dtos.json Job.counts). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int files;
  int warnings;
  int errors;
  int read_errors;
} BroJobTotals;

G_END_DECLS
