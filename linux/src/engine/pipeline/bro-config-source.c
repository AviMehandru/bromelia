/* bro-config-source.c */
#include "bro-config-source.h"

G_DEFINE_INTERFACE (BroConfigSource, bro_config_source, G_TYPE_OBJECT)

static void
bro_config_source_default_init (BroConfigSourceInterface *iface)
{
}

BroConfig *
bro_config_source_current (BroConfigSource *self)
{
  return BRO_CONFIG_SOURCE_GET_IFACE (self)->current (self);
}
