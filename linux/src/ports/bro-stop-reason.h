/* bro-stop-reason.h: why a process is stopped. Only cancelled and shutdown make its exit cancelled; policy is the
 * engine's own decision (MakeMKV's space warning, a renumbered drive). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_STOP_REASON_CANCELLED,
  BRO_STOP_REASON_STALLED,
  BRO_STOP_REASON_TIMED_OUT,
  BRO_STOP_REASON_SHUTDOWN,
  BRO_STOP_REASON_POLICY,
} BroStopReason;

/* The wire form ("cancelled"). */
const char *bro_stop_reason_to_wire (BroStopReason value);
gboolean bro_stop_reason_from_wire (const char *text, BroStopReason *out);

G_END_DECLS
