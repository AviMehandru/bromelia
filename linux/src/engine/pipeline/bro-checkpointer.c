/* bro-checkpointer.c */
#include "bro-checkpointer.h"

G_DEFINE_INTERFACE (BroCheckpointer, bro_checkpointer, G_TYPE_OBJECT)

static void
bro_checkpointer_default_init (BroCheckpointerInterface *iface)
{
}

BroJsonValue *
bro_checkpointer_saved (BroCheckpointer *self)
{
  return BRO_CHECKPOINTER_GET_IFACE (self)->saved (self);
}

gboolean
bro_checkpointer_save (BroCheckpointer *self, const BroJsonValue *checkpoint, BroBroError **error)
{
  return BRO_CHECKPOINTER_GET_IFACE (self)->save (self, checkpoint, error);
}
