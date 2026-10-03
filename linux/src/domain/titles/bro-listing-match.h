/* bro-listing-match.h: BroListingMatch, matches the titles of one listing to another: the listing a disc was
 * opened with and the listing read when the job starts, or a disc and its backup. MakeMKV numbers titles by
 * position, which changes with the minimum length setting, so titles are matched by source title, duration and
 * segment map. */
#pragma once

#include "bro-bro-error.h"
#include "bro-bro-message.h"
#include "bro-listing.h"

G_BEGIN_DECLS

/* Why @new_listing looks like another disc than @old (disc.changed.volume / disc.changed.titles), or NULL when
 * it may be the same disc. */
BroBroMessage *bro_listing_match_different_disc (const BroListing *old, const BroListing *new_listing);

/* Maps each of @indices (int, titles of @old) to a title of @new_listing, preferring the same number: a table of
 * GINT_TO_POINTER (old) → GINT_TO_POINTER (new). NULL and @error set when a title is missing
 * (disc.titleNotInListing, disc.titleGone), or when @same_tracks (nullable set of GINT_TO_POINTER (index))
 * holds it and its track list changed (disc.tracksChanged). */
GHashTable *bro_listing_match_map_titles (GArray *indices, const BroListing *old, const BroListing *new_listing,
                                          GHashTable *same_tracks, BroBroError **error);

G_END_DECLS
