/* bro-message-renderer.c */
#include "bro-message-renderer.h"

G_DEFINE_INTERFACE (BroMessageRenderer, bro_message_renderer, G_TYPE_OBJECT)

static void
bro_message_renderer_default_init (BroMessageRendererInterface *iface)
{
}

char *
bro_message_renderer_render (BroMessageRenderer *self, const BroBroMessage *message)
{
  g_return_val_if_fail (BRO_IS_MESSAGE_RENDERER (self), NULL);
  return BRO_MESSAGE_RENDERER_GET_IFACE (self)->render (self, message);
}
