/* bro-tool-run.c */
#include "bro-tool-run.h"

static void
stop_on_cancel (gpointer process, gpointer unused)
{
  bro_running_process_stop (process, BRO_STOP_REASON_CANCELLED);
}

gboolean
bro_tool_run (BroProcessLauncher *launcher, const BroProcessSpec *spec, BroCancellationToken *cancel, BroToolRunStartedFunc started,
              BroToolRunLineFunc on_line, gpointer data, BroProcessExit *exit, BroBroMessage **transcript_problem, BroBroError **error)
{
  g_autoptr (BroRunningProcess) process = bro_process_launcher_start (launcher, spec, error);
  gboolean stopped = FALSE;
  BroOutputLine *line;
  guint handler;
  if (!process)
    return FALSE;
  handler = cancel ? bro_cancellation_token_on_cancel (cancel, stop_on_cancel, process, NULL) : 0;
  if (started)
    started (process, data);
  while ((line = bro_running_process_lines (process)) != NULL)
    {
      BroStopReason reason = BRO_STOP_REASON_POLICY;
      if (on_line && on_line (line, data, &reason) && !stopped)
        {
          stopped = TRUE;
          bro_running_process_stop (process, reason);
        }
      bro_output_line_free (line);
    }
  *exit = bro_running_process_wait (process);
  if (transcript_problem)
    *transcript_problem = bro_running_process_transcript_problem (process);
  if (handler)
    bro_cancellation_token_disconnect (cancel, handler);
  return TRUE;
}
