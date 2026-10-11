/* bro-job-logger.c */
#include "bro-job-logger.h"

G_DEFINE_INTERFACE (BroJobLogger, bro_job_logger, G_TYPE_OBJECT)

static void
bro_job_logger_default_init (BroJobLoggerInterface *iface)
{
}

void
bro_job_logger_log (BroJobLogger *self, const BroBroMessage *message)
{
  BRO_JOB_LOGGER_GET_IFACE (self)->log (self, message);
}
