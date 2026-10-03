/* bro-test-english.h: renders a message {code, params} in English from shared/messages/en.json, for fixtures that
 * check a message's text. The subset of ICU MessageFormat en.json uses: {x}, plural (with =N, one, other and #),
 * select, bytes, duration, durationPrecise, and nested messages (params of type message / messages). Apostrophes
 * are plain text. (The engine host's MessageRenderer, in Adapters, will do this properly.) */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

char *bro_test_english (const BroJsonValue *message);

G_END_DECLS
