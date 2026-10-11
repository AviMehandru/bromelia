/* bro-job-counts.h: a count per queue. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int acquisition;
  int processing;
  int maintenance;
} BroJobCounts;

G_END_DECLS
