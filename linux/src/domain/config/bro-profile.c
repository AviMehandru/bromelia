/* bro-profile.c */
#include "bro-profile.h"

BroProfile *
bro_profile_new (BroJsonValue *json)
{
  BroProfile *p = g_new0 (BroProfile, 1);
  p->json = bro_json_value_ref (json);
  return p;
}

void
bro_profile_free (BroProfile *profile)
{
  if (!profile)
    return;
  bro_json_value_unref (profile->json);
  g_free (profile);
}
