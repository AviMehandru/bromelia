/* bro-tool-locator.c */
#include "bro-tool-locator.h"

G_DEFINE_INTERFACE (BroToolLocator, bro_tool_locator, G_TYPE_OBJECT)

static void
bro_tool_locator_default_init (BroToolLocatorInterface *iface)
{
}

BroToolInfo *
bro_tool_locator_locate (BroToolLocator *self, BroToolKind tool)
{
  g_return_val_if_fail (BRO_IS_TOOL_LOCATOR (self), NULL);
  return BRO_TOOL_LOCATOR_GET_IFACE (self)->locate (self, tool);
}
