/* bro-tool-locator.h: BroToolLocator: Finds the external tools. */
#pragma once

#include "bro-tool-info.h"
#include "bro-tool-kind.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_TOOL_LOCATOR (bro_tool_locator_get_type ())
G_DECLARE_INTERFACE (BroToolLocator, bro_tool_locator, BRO, TOOL_LOCATOR, GObject)

struct _BroToolLocatorInterface {
  GTypeInterface parent_iface;

  BroToolInfo *(*locate) (BroToolLocator *self, BroToolKind tool);
};

BroToolInfo *bro_tool_locator_locate (BroToolLocator *self, BroToolKind tool);

G_END_DECLS
