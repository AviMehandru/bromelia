/* bro-id-generator.c */
#include "bro-id-generator.h"

G_DEFINE_INTERFACE (BroIdGenerator, bro_id_generator, G_TYPE_OBJECT)

static void
bro_id_generator_default_init (BroIdGeneratorInterface *iface)
{
}

BroId
bro_id_generator_next (BroIdGenerator *self)
{
  BroId none = { { 0 } };
  g_return_val_if_fail (BRO_IS_ID_GENERATOR (self), none);
  return BRO_ID_GENERATOR_GET_IFACE (self)->next (self);
}
