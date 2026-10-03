/* bro-listing.c */
#include "bro-listing.h"

BroListing *
bro_listing_ref (BroListing *listing)
{
  return g_atomic_rc_box_acquire (listing);
}

static void
clear_listing (BroListing *l)
{
  g_free (l->name);
  g_free (l->volume_name);
  g_free (l->type_text);
  g_ptr_array_unref (l->titles);
  g_hash_table_unref (l->attributes);
}

void
bro_listing_unref (BroListing *listing)
{
  if (listing)
    g_atomic_rc_box_release_full (listing, (GDestroyNotify) clear_listing);
}
