/* bro-listing-run.h: an info run: the listing it printed and the run. */
#pragma once

#include "bro-listing.h"
#include "bro-makemkv-run.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroListing *listing;
  BroMakemkvRun *run;
} BroListingRun;

/* Everything zero. */
BroListingRun *bro_listing_run_new (void);
void bro_listing_run_free (BroListingRun *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroListingRun, bro_listing_run_free)

G_END_DECLS
