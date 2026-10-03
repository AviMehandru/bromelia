/* bro-process-exit.h: how a process ended: its exit status, the signal that ended it, the silence that stopped
 * it (stalled_seconds < 0: it didn't stall), whether it was abandoned after KILL, and whether it was cancelled. */
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
