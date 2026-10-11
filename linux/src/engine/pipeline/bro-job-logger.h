/* bro-job-logger.h: BroJobLogger: How a step logs, from its worker: a jobLog event at once; jobs/<id>/log.jsonl when the step ends. */
#pragma once

#include "bro-bro-message.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_JOB_LOGGER (bro_job_logger_get_type ())
G_DECLARE_INTERFACE (BroJobLogger, bro_job_logger, BRO, JOB_LOGGER, GObject)

struct _BroJobLoggerInterface {
  GTypeInterface parent_iface;
  void (*log) (BroJobLogger *self, const BroBroMessage *message);
};

/* @message is copied. */
void bro_job_logger_log (BroJobLogger *self, const BroBroMessage *message);

G_END_DECLS
