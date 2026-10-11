/* bro-listing-run.c */
#include "bro-listing-run.h"

BroListingRun *
bro_listing_run_new (void)
{
  return g_new0 (BroListingRun, 1);
}

void
bro_listing_run_free (BroListingRun *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->listing, bro_listing_unref);
  g_clear_pointer (&x->run, bro_makemkv_run_free);
  g_free (x);
}
