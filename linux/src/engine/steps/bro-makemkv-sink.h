/* bro-makemkv-sink.h: BroMakemkvSink: turns what makemkvcon says into the job's progress and log: every message but
 * debug ones as makemkv.message with its severity, the total progress as the step's fraction (under the given
 * operation, with MakeMKV's current item), and each new total operation as makemkv.titleProgress. A BroRunSink;
 * thread-safe. */
#pragma once

#include "bro-job-logger.h"
#include "bro-run-outcome.h"
#include "bro-step-context.h"
#include "bro-progress-reporter.h"
#include "bro-run-sink.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MAKEMKV_SINK (bro_makemkv_sink_get_type ())
G_DECLARE_FINAL_TYPE (BroMakemkvSink, bro_makemkv_sink, BRO, MAKEMKV_SINK, GObject)

/* Keeps references to @progress and @log; copies @operation. */
BroMakemkvSink *bro_makemkv_sink_new (BroProgressReporter *progress, BroJobLogger *log, const BroBroMessage *operation);

/* MakeMKV starts its debug log (app_ShowDebug, named by message 1004) afresh at every start, so after each run the
 * steps add it (BroRunOutcome's debug_log) to the job folder's makemkv-debug.txt (BRO_SEAL_STEP_DEBUG_TRANSCRIPT) of
 * the BroStepContext, after a line "==== <its path> after $ makemkvcon <command>" (the subcommand: info, mkv, backup).
 * Nothing when the run named none or it is empty; a file that can't be read or written is left out (the run's result
 * stands). */
void bro_makemkv_sink_keep_debug_log (const BroStepContext *context, const BroRunOutcome *outcome, const char *command);

G_END_DECLS
