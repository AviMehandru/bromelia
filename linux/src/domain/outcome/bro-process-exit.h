/* bro-process-exit.h: how a process ended: its exit status, the signal that ended it, the silence that stopped
 * it (stalled_seconds < 0: it didn't stall), whether it was abandoned after KILL (it may still be running and writing:
 * what it wrote to must be quarantined, never reused or removed), and whether it was cancelled (by the user or a
 * shutdown, not by the engine's own decision). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int status;
  int signal;             /* 0: none */
  double stalled_seconds; /* < 0: didn't stall */
  gboolean abandoned;
  gboolean cancelled;
} BroProcessExit;

/* An exit with @status and nothing else. */
BroProcessExit bro_process_exit_status (int status);

G_END_DECLS
