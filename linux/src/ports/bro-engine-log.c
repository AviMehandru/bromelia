/* bro-engine-log.c */
#include "bro-engine-log.h"

G_DEFINE_INTERFACE (BroEngineLog, bro_engine_log, G_TYPE_OBJECT)

static void
bro_engine_log_default_init (BroEngineLogInterface *iface)
{
}

void
bro_engine_log_log (BroEngineLog *self, const BroBroMessage *message)
{
  g_return_if_fail (BRO_IS_ENGINE_LOG (self));
  BRO_ENGINE_LOG_GET_IFACE (self)->log (self, message);
}
