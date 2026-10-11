/* bro-progress-reporter.c */
#include "bro-progress-reporter.h"

G_DEFINE_INTERFACE (BroProgressReporter, bro_progress_reporter, G_TYPE_OBJECT)

static void
bro_progress_reporter_default_init (BroProgressReporterInterface *iface)
{
}

void
bro_progress_reporter_report (BroProgressReporter *self, double fraction, const BroBroMessage *operation, const char *current)
{
  BRO_PROGRESS_REPORTER_GET_IFACE (self)->report (self, fraction, operation, current);
}
