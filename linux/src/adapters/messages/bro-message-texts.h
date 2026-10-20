/* bro-message-texts.h: BroMessageTexts: one language's message texts (ICU MessageFormat, shared/messages/<lang>.json)
 * by code, and the params codes.json declares as messageList (rendered joined with ", "; other message lists with
 * "; "). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BroMessageTexts BroMessageTexts;

/* An empty catalog; free with bro_message_texts_free. */
BroMessageTexts *bro_message_texts_new (void);
void bro_message_texts_free (BroMessageTexts *self);

/* Adds (or replaces) a code's text. */
void bro_message_texts_add_text (BroMessageTexts *self, const char *code, const char *text);

/* Marks a code's param as a messageList. */
void bro_message_texts_add_message_list (BroMessageTexts *self, const char *code, const char *param);

/* The text of a code (borrowed); NULL when the catalog has none. */
const char *bro_message_texts_text (const BroMessageTexts *self, const char *code);

/* Whether a code's param is a messageList. */
gboolean bro_message_texts_is_message_list (const BroMessageTexts *self, const char *code, const char *param);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMessageTexts, bro_message_texts_free)

G_END_DECLS
