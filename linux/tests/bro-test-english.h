/* bro-test-english.h: renders a message {code, params, cause} in English, for fixtures that check a message's text: the
 * host's BroMessageTextRenderer over the generated English catalog (shared/messages/en.json). */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

char *bro_test_english (const BroJsonValue *message);

G_END_DECLS
