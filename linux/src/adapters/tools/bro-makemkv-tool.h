/* bro-makemkv-tool.h: BroMakemkvTool: makemkvcon (plan §10.1). Every call prepares the run's settings
 * (SettingsIsolation), builds the arguments (MakemkvArgs), starts it (ProcessLauncher; TERM first, since makemkvcon
 * ignores INT), feeds every line to Robot.parseLine, the RunAccumulator and the sink, gives the settings back on the
 * first line, stops it when the accumulator says so (MakeMKV's space warning, a renumbered drive) or when cancelled,
 * and classifies the run with the names that are new in the destination (bro_run_outcome_products picks what counts).
 * A destination that can't be listed before or after the run fails the call: every name in it would otherwise look
 * new, or none. Holds no state between calls. Synchronous: call it from a worker thread (plan §6). */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-file-system.h"
#include "bro-listing-run.h"
#include "bro-makemkv-drive.h"
#include "bro-makemkv-invocation.h"
#include "bro-makemkv-run.h"
#include "bro-makemkv-source.h"
#include "bro-process-launcher.h"
#include "bro-run-sink.h"
#include "bro-settings-isolation.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MAKEMKV_TOOL (bro_makemkv_tool_get_type ())
G_DECLARE_FINAL_TYPE (BroMakemkvTool, bro_makemkv_tool, BRO, MAKEMKV_TOOL, GObject)

/* Keeps references to the ports. */
BroMakemkvTool *bro_makemkv_tool_new (BroProcessLauncher *launcher, BroFileSystem *fs, BroSettingsIsolation *isolation,
                                      BroToolLocator *locator);

/* -r --cache=1 info disc:9999, without settings isolation: every DRV line (BroMakemkvDrive *). NULL and @error set
 * when makemkvcon can't be run. @cancel may be NULL. */
GPtrArray *bro_makemkv_tool_scan_drives (BroMakemkvTool *self, BroCancellationToken *cancel, BroBroError **error);

BroListingRun *bro_makemkv_tool_listing (BroMakemkvTool *self, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation,
                                         BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error);

/* @title: a title index or "all". */
BroMakemkvRun *bro_makemkv_tool_rip (BroMakemkvTool *self, const BroMakemkvSource *source, const char *title, const char *destination,
                                     const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel,
                                     BroBroError **error);

/* disc:N only (backup.needsDrive otherwise). Stops at once when a DRV line shows the index now names another device. */
BroMakemkvRun *bro_makemkv_tool_backup (BroMakemkvTool *self, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
                                        const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel,
                                        BroBroError **error);

G_END_DECLS
