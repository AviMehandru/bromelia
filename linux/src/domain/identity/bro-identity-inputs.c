/* bro-identity-inputs.c */
#include "bro-identity-inputs.h"

BroIdentityInputs *
bro_identity_inputs_new (BroListing *listing, const char *disc_label, gboolean encrypted)
{
  BroIdentityInputs *i = g_new0 (BroIdentityInputs, 1);
  i->listing = listing ? bro_listing_ref (listing) : NULL;
  i->disc_label = g_strdup (disc_label ? disc_label : "");
  i->encrypted = encrypted;
  i->name_override = g_strdup ("");
  return i;
}

void
bro_identity_inputs_free (BroIdentityInputs *i)
{
  if (!i)
    return;
  bro_listing_unref (i->listing);
  g_free (i->disc_label);
  g_free (i->name_override);
  g_free (i);
}
