/* bro-decision-port.h: BroDecisionPort: How a step asks the user, from its worker: the job is awaitingDecision until
 * one of the answers is given. */
#pragma once

#include "bro-bro-error.h"
#include "bro-decision-dto.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_DECISION_PORT (bro_decision_port_get_type ())
G_DECLARE_INTERFACE (BroDecisionPort, bro_decision_port, BRO, DECISION_PORT, GObject)

struct _BroDecisionPortInterface {
  GTypeInterface parent_iface;
  char *(*ask) (BroDecisionPort *self, const BroDecisionDto *decision, BroBroError **error);
};

/* Waits for one of the answers and returns its id (g_free); NULL and @error set (job.cancelled) when the job is
 * cancelled first. */
char *bro_decision_port_ask (BroDecisionPort *self, const BroDecisionDto *decision, BroBroError **error);

G_END_DECLS
