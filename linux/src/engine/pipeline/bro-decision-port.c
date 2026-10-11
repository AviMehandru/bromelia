/* bro-decision-port.c */
#include "bro-decision-port.h"

G_DEFINE_INTERFACE (BroDecisionPort, bro_decision_port, G_TYPE_OBJECT)

static void
bro_decision_port_default_init (BroDecisionPortInterface *iface)
{
}

char *
bro_decision_port_ask (BroDecisionPort *self, const BroDecisionDto *decision, BroBroError **error)
{
  return BRO_DECISION_PORT_GET_IFACE (self)->ask (self, decision, error);
}
