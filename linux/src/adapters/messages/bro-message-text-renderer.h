/* bro-message-text-renderer.h: BroMessageTextRenderer: the host's BroMessageRenderer (plan §16): a message {code,
 * params, cause} as text, from a catalog of ICU MessageFormat texts (BroMessageTexts). The subset shared/messages
 * uses: {x}; plural with =N, one, other and #; select with other; the custom formats bytes, duration and
 * durationPrecise (codes.json's paramTypes). Params of type message, messages and messageList are rendered from the
 * same catalog, and a cause follows as " (cause)". Apostrophes follow ICU: '' is one, and one before { or } (or # in a
 * plural) quotes the text up to the next. It never fails: a missing param shows as {name}, an unknown code as the
 * code, and an unclosed brace as text. Any thread. */
#pragma once

#include "bro-message-renderer.h"
#include "bro-message-texts.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MESSAGE_TEXT_RENDERER (bro_message_text_renderer_get_type ())
G_DECLARE_FINAL_TYPE (BroMessageTextRenderer, bro_message_text_renderer, BRO, MESSAGE_TEXT_RENDERER, GObject)

/* Takes ownership of @texts. */
BroMessageTextRenderer *bro_message_text_renderer_new (BroMessageTexts *texts);

/* The port: a BroMessage's text (yours to free). */
char *bro_message_text_renderer_render (BroMessageTextRenderer *self, const BroBroMessage *message);

/* The text of a message in its JSON form {code, params, cause}: a BroError's, or a nested message's (yours to free). */
char *bro_message_text_renderer_render_json (BroMessageTextRenderer *self, const BroJsonValue *message);

G_END_DECLS
