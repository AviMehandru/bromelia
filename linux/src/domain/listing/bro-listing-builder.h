/* bro-listing-builder.h: builds a BroListing from robot events (TCOUNT, CINFO, TINFO, SINFO; others are ignored). */
#pragma once

#include "bro-listing.h"
#include "bro-robot-event.h"

G_BEGIN_DECLS

typedef struct _BroListingBuilder BroListingBuilder;

BroListingBuilder *bro_listing_builder_new (void);
void bro_listing_builder_free (BroListingBuilder *builder);

void bro_listing_builder_feed (BroListingBuilder *builder, const BroRobotEvent *event);
BroListing *bro_listing_builder_build (BroListingBuilder *builder);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroListingBuilder, bro_listing_builder_free)

G_END_DECLS
