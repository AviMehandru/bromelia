/* bro-message-renderer.h: BroMessageRenderer: the text of a message in the configured language (plan §16:
 * shared/messages/en.json, rendered by the host's adapter): for what the engine writes for people to read, such as the
 * INCOMPLETE.txt note (owner, 2026-10-14). */
#pragma once

#include "bro-bro-message.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MESSAGE_RENDERER (bro_message_renderer_get_type ())
G_DECLARE_INTERFACE (BroMessageRenderer, bro_message_renderer, BRO, MESSAGE_RENDERER, GObject)

struct _BroMessageRendererInterface {
  GTypeInterface parent_iface;

  char *(*render) (BroMessageRenderer *self, const BroBroMessage *message);
};

/* The text (yours to free). */
char *bro_message_renderer_render (BroMessageRenderer *self, const BroBroMessage *message);

G_END_DECLS
