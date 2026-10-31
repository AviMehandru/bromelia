/* bro-engine-log.h: BroEngineLog: where the engine writes its own log lines (plan §16), from any thread:
 * recovery.summary at start. The host's adapter writes logs/engine.log with each line's time; rotation and the secret
 * redaction come with bromeliad (phase 4). */
#pragma once

#include "bro-bro-message.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ENGINE_LOG (bro_engine_log_get_type ())
G_DECLARE_INTERFACE (BroEngineLog, bro_engine_log, BRO, ENGINE_LOG, GObject)

struct _BroEngineLogInterface {
  GTypeInterface parent_iface;

  void (*log) (BroEngineLog *self, const BroBroMessage *message);
};

/* Appends the line (time, severity, code, params); a failure to write is dropped (logging never fails the engine). */
void bro_engine_log_log (BroEngineLog *self, const BroBroMessage *message);

G_END_DECLS
