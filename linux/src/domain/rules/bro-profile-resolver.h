/* bro-profile-resolver.h: BroProfileResolver, the effective profile of a disc (plan §24). */
#pragma once

#include "bro-config.h"
#include "bro-effective-profile.h"
#include "bro-rule-facts.h"

G_BEGIN_DECLS

/* Layers, lowest first, each merged like an RFC 7386 merge patch (null removes a key, so its default comes back):
 * every field at its default → the default profile → the drive's profile → the drive's own MakeMKV settings → each
 * enabled rule that matches, in order (its then.set; then.profile replaces the drive's profile and later rules still
 * apply) → the session's choices (nullable). Steps are the profile's, then the rules', each once. @drive_id is the
 * drive entry's id (NULL: no entry, or an ISO / folder). */
BroEffectiveProfile *bro_profile_resolver_resolve (const BroConfig *config, const char *drive_id, const BroRuleFacts *facts,
                                                   BroJsonValue *session_choices);

G_END_DECLS
