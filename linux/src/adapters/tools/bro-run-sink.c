/* bro-run-sink.c */
#include "bro-run-sink.h"

G_DEFINE_INTERFACE (BroRunSink, bro_run_sink, G_TYPE_OBJECT)

static void
bro_run_sink_default_init (BroRunSinkInterface *iface)
{
}

void
bro_run_sink_event (BroRunSink *self, const BroRobotEvent *event)
{
  g_return_if_fail (BRO_IS_RUN_SINK (self));
  BRO_RUN_SINK_GET_IFACE (self)->event (self, event);
}
