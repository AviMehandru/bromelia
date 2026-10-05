/* bro-running-process.h: BroRunningProcess: A started process. stop escalates (INT →) TERM → KILL, 5 s apart, and
 * abandons the process 30 s after KILL. An abandoned process (BroProcessExit.abandoned) may still be running and
 * writing: whatever it was writing to (a staging folder, an image) must be quarantined, never reused or removed. */
#pragma once

#include "bro-bro-message.h"
#include "bro-output-line.h"
#include "bro-process-exit.h"
#include "bro-stop-reason.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_RUNNING_PROCESS (bro_running_process_get_type ())
G_DECLARE_INTERFACE (BroRunningProcess, bro_running_process, BRO, RUNNING_PROCESS, GObject)

struct _BroRunningProcessInterface {
  GTypeInterface parent_iface;

  BroOutputLine *(*lines) (BroRunningProcess *self);
  BroProcessExit (*wait) (BroRunningProcess *self);
  void (*stop) (BroRunningProcess *self, BroStopReason reason);
  BroBroMessage *(*transcript_problem) (BroRunningProcess *self); /* NULL: never a problem */
};

/* Its output lines, in order, until it closes its output. The next line, waiting for it; NULL once the process has
 * closed its output. */
BroOutputLine *bro_running_process_lines (BroRunningProcess *self);

/* Waits for it to end. */
BroProcessExit bro_running_process_wait (BroRunningProcess *self);

/* Stops it (see above); wait reports the reason. */
void bro_running_process_stop (BroRunningProcess *self, BroStopReason reason);

/* process.noTranscript when the transcript the spec asked for couldn't be opened, or a line couldn't be written to it
 * (the first failure); NULL otherwise. Free with bro_bro_message_free. */
BroBroMessage *bro_running_process_transcript_problem (BroRunningProcess *self);

G_END_DECLS
