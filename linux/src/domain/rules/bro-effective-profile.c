/* bro-effective-profile.c */
#include "bro-effective-profile.h"

BroEffectiveProfile *
bro_effective_profile_new (BroJsonValue *profile, GStrv steps, GPtrArray *trace)
{
  BroEffectiveProfile *e = g_new0 (BroEffectiveProfile, 1);
  e->profile = profile;
  e->steps = steps ? steps : g_new0 (char *, 1);
  e->trace = trace;
  return e;
}

void
bro_effective_profile_free (BroEffectiveProfile *effective)
{
  if (!effective)
    return;
  g_clear_pointer (&effective->profile, bro_json_value_unref);
  g_strfreev (effective->steps);
  g_clear_pointer (&effective->trace, g_ptr_array_unref);
  g_free (effective);
}
