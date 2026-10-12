/* bro-makemkv-sink.h: BroMakemkvSink: turns what makemkvcon says into the job's progress and log: every message but
 * debug ones as makemkv.message with its severity, the total progress as the step's fraction (under the given
 * operation, with MakeMKV's current item), and each new total operation as makemkv.titleProgress. A BroRunSink;
 * thread-safe. */
#pragma once

#include "bro-job-logger.h"
#include "bro-progress-reporter.h"
#include "bro-run-sink.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MAKEMKV_SINK (bro_makemkv_sink_get_type ())
G_DECLARE_FINAL_TYPE (BroMakemkvSink, bro_makemkv_sink, BRO, MAKEMKV_SINK, GObject)

/* Keeps references to @progress and @log; copies @operation. */
BroMakemkvSink *bro_makemkv_sink_new (BroProgressReporter *progress, BroJobLogger *log, const BroBroMessage *operation);

G_END_DECLS
