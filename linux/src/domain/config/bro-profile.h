/* bro-profile.h: a profile: sparse, as written in the configuration (a missing field is inherited; see
 * BroProfileResolver), so it stays a JSON object; its id and name are json's "id" and "name" (no id for a
 * template not added yet). */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  BroJsonValue *json;
} BroProfile;

/* Takes a reference to @json. */
BroProfile *bro_profile_new (BroJsonValue *json);
void bro_profile_free (BroProfile *profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroProfile, bro_profile_free)

G_END_DECLS
