/* bro-progress-reporter.h: BroProgressReporter: How a step reports its progress, from its worker. The runner coalesces it (one update per 100 ms). */
#pragma once

#include "bro-bro-message.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PROGRESS_REPORTER (bro_progress_reporter_get_type ())
G_DECLARE_INTERFACE (BroProgressReporter, bro_progress_reporter, BRO, PROGRESS_REPORTER, GObject)

struct _BroProgressReporterInterface {
  GTypeInterface parent_iface;
  void (*report) (BroProgressReporter *self, double fraction, const BroBroMessage *operation, const char *current);
};

/* @fraction of the step (0–1), what it does (copied), the item it works on (nullable). */
void bro_progress_reporter_report (BroProgressReporter *self, double fraction, const BroBroMessage *operation, const char *current);

G_END_DECLS
