/* bro-stop-policy.h: how a process is stopped: TERM first (makemkvcon ignores INT), or INT first (the others). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_STOP_POLICY_TERMINATE_FIRST,
  BRO_STOP_POLICY_INTERRUPT_FIRST,
} BroStopPolicy;

/* The wire form ("terminateFirst"). */
const char *bro_stop_policy_to_wire (BroStopPolicy value);
gboolean bro_stop_policy_from_wire (const char *text, BroStopPolicy *out);

G_END_DECLS
