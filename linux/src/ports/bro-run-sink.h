/* bro-run-sink.h: BroRunSink: where a tool reports what makemkvcon says, event by event (progress, messages, the
 * current title). Called on the tool's thread. */
#pragma once

#include "bro-robot-event.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_RUN_SINK (bro_run_sink_get_type ())
G_DECLARE_INTERFACE (BroRunSink, bro_run_sink, BRO, RUN_SINK, GObject)

struct _BroRunSinkInterface {
  GTypeInterface parent_iface;

  void (*event) (BroRunSink *self, const BroRobotEvent *event);
};

void bro_run_sink_event (BroRunSink *self, const BroRobotEvent *event);

G_END_DECLS
