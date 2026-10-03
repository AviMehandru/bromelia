/* bro-rule-matcher.h: BroRuleMatcher, whether a rule's conditions hold for a disc (was PluginMatcher). */
#pragma once

#include "bro-rule-facts.h"
#include "bro-rule-when.h"

G_BEGIN_DECLS

/* Every condition given must hold; an empty @when matches every disc. The regular expressions are case-insensitive
 * (an invalid one never matches); a format ending in * matches every code with that prefix (BR* = BR and BRe),
 * others match exactly, ignoring case. */
gboolean bro_rule_matcher_matches (const BroRuleWhen *when, const BroRuleFacts *facts);

G_END_DECLS
