/* bro-title-rules.h: BroTitleRules, which titles of a listing to rip (today's TitleSelector). */
#pragma once

#include "bro-listing.h"
#include "bro-selection.h"
#include "bro-title-settings.h"

G_BEGIN_DECLS

/* Filters, then duplicates (same segment map, length and angle), then the strategy, then max_titles. Every
 * title gets a reason. */
BroSelection *bro_title_rules_select (const BroListing *listing, const BroTitleSettings *rules);

G_END_DECLS
