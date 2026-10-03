/* bro-rule.h: a rule of the configuration (config-3.json's Rule): when → then. Replaces plugins, formatModes and
 * matchName / matchFormats. */
#pragma once

#include "bro-rule-then.h"
#include "bro-rule-when.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *name;
  gboolean enabled;
  BroRuleWhen when;
  BroRuleThen then;
} BroRule;

BroRule *bro_rule_decode (BroJsonValue *json);
void bro_rule_free (BroRule *rule);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRule, bro_rule_free)

G_END_DECLS
