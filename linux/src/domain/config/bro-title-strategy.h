/* bro-title-strategy.h: how titles are chosen after the filters */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_TITLE_STRATEGY_ALL,
  BRO_TITLE_STRATEGY_LONGEST,
  BRO_TITLE_STRATEGY_INDICES,
  BRO_TITLE_STRATEGY_MANUAL,
} BroTitleStrategy;

/* The wire form ("all"), as in shared/schema/common.json. */
const char *bro_title_strategy_to_wire (BroTitleStrategy value);
gboolean bro_title_strategy_from_wire (const char *text, BroTitleStrategy *out);

G_END_DECLS
