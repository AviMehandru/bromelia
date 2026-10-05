/* bro-tool-run.h: how every tool adapter runs its process (Swift and C#: ToolRun). */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-process-launcher.h"

G_BEGIN_DECLS

/* Called for each output line; TRUE and *@reason set to stop the process (acted on once). */
typedef gboolean (*BroToolRunLineFunc) (const BroOutputLine *line, gpointer data, BroStopReason *reason);

/* Called once the process has started (a timer that stops it, for instance). */
typedef void (*BroToolRunStartedFunc) (BroRunningProcess *process, gpointer data);

/* Starts @spec, hands each output line to @on_line (NULL: the lines are read and dropped), stops the process when
 * @cancel (nullable) is cancelled, and returns once it has ended, with *@exit saying how and *@transcript_problem
 * (nullable) the process's transcript problem (bro_running_process_transcript_problem). FALSE and @error set when it
 * can't start. */
gboolean bro_tool_run (BroProcessLauncher *launcher, const BroProcessSpec *spec, BroCancellationToken *cancel, BroToolRunStartedFunc started,
                       BroToolRunLineFunc on_line, gpointer data, BroProcessExit *exit, BroBroMessage **transcript_problem,
                       BroBroError **error);

G_END_DECLS
