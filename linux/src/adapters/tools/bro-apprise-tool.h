/* bro-apprise-tool.h: BroAppriseTool: the apprise command (plan §10.1): apprise -t title -b body url.
 * notify.needsApprise when the locator can't find it (naming the URL's scheme: URLs are secrets), notify.appriseFailed
 * when it exits non-zero. Synchronous. */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-process-launcher.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_APPRISE_TOOL (bro_apprise_tool_get_type ())
G_DECLARE_FINAL_TYPE (BroAppriseTool, bro_apprise_tool, BRO, APPRISE_TOOL, GObject)

/* Keeps references to the ports. */
BroAppriseTool *bro_apprise_tool_new (BroProcessLauncher *launcher, BroToolLocator *locator);

gboolean bro_apprise_tool_send (BroAppriseTool *self, const char *url, const char *title, const char *body, BroCancellationToken *cancel,
                                BroBroError **error);

/* The URL's scheme ("mailto"), or the whole value when it has none (then it isn't a secret URL). Free with g_free. */
char *_bro_apprise_tool_scheme (const char *url);

G_END_DECLS
