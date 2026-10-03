/* bro-effective-profile.h: the profile a disc gets: every field set (JSON, config-3.json's ProfileFields), the steps
 * to run (the profile's, then those rules add), and where each part came from. */
#pragma once

#include "bro-json-value.h"
#include "bro-resolve-trace.h"

G_BEGIN_DECLS

typedef struct {
  BroJsonValue *profile;
  GStrv steps;      /* never NULL */
  GPtrArray *trace; /* BroResolveTrace * */
} BroEffectiveProfile;

/* Takes ownership of all three. */
BroEffectiveProfile *bro_effective_profile_new (BroJsonValue *profile, GStrv steps, GPtrArray *trace);
void bro_effective_profile_free (BroEffectiveProfile *effective);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEffectiveProfile, bro_effective_profile_free)

G_END_DECLS
