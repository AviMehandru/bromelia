/* bro-rule-then.h: what a matching rule does: switch to a profile, merge a profile patch (an RFC 7386 merge
 * patch), add steps after the profile's. Part of BroRule. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  char *profile;      /* nullable */
  BroJsonValue *set;  /* nullable */
  GStrv steps;        /* never NULL */
} BroRuleThen;

G_END_DECLS
