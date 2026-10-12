/* bro-probe-output.c */
#include "bro-probe-output.h"

BroProbeOutput *
bro_probe_output_new (BroListing *listing, const char *fingerprint, const char *libre_drive, gboolean from_session)
{
  BroProbeOutput *o = g_new0 (BroProbeOutput, 1);
  o->listing = listing ? bro_listing_ref (listing) : NULL;
  o->fingerprint = g_strdup (fingerprint);
  o->libre_drive = g_strdup (libre_drive);
  o->from_session = from_session;
  return o;
}

void
bro_probe_output_free (BroProbeOutput *output)
{
  if (!output)
    return;
  bro_listing_unref (output->listing);
  g_free (output->fingerprint);
  g_free (output->libre_drive);
  g_free (output);
}

static BroJsonValue *
string_or_null (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

BroJsonValue *
bro_probe_output_encode (const BroProbeOutput *output)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "listing", output->listing ? bro_listing_encode (output->listing) : bro_json_value_new_null ());
  bro_json_value_set (o, "fingerprint", string_or_null (output->fingerprint));
  bro_json_value_set (o, "libreDrive", string_or_null (output->libre_drive));
  bro_json_value_set (o, "fromSession", bro_json_value_new_bool (output->from_session));
  return o;
}

BroProbeOutput *
bro_probe_output_decode (const BroJsonValue *json)
{
  BroJsonValue *l = bro_json_value_member (json, "listing");
  g_autoptr (BroListing) listing = l && l->kind == BRO_JSON_VALUE_OBJECT ? bro_listing_decode (l) : NULL;
  return bro_probe_output_new (listing, bro_json_value_get_string (bro_json_value_member (json, "fingerprint"), NULL),
                               bro_json_value_get_string (bro_json_value_member (json, "libreDrive"), NULL),
                               bro_json_value_get_bool (bro_json_value_member (json, "fromSession"), FALSE));
}
