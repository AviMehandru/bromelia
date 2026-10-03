/* bro-listing.h: what makemkvcon reported about a disc, ISO or folder: its name (2, else 32), volume name (32),
 * type (from 1), the number of titles it announced, the titles in index order, and every disc attribute.
 * Reference counted; don't change one that's shared. */
#pragma once

#include "bro-disc-type.h"
#include "bro-title.h"

G_BEGIN_DECLS

typedef struct {
  char *name;
  char *volume_name;
  BroDiscType type;
  char *type_text;
  int reported_title_count;
  GPtrArray *titles;      /* BroTitle *, in index order */
  GHashTable *attributes; /* GINT_TO_POINTER (BroAttributeId) → char * */
} BroListing;

BroListing *bro_listing_ref (BroListing *listing);
void bro_listing_unref (BroListing *listing);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroListing, bro_listing_unref)

G_END_DECLS
