/* bro-conflict-policy.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_CONFLICT_POLICY_NEW_FOLDER,
  BRO_CONFLICT_POLICY_MERGE,
  BRO_CONFLICT_POLICY_SKIP,
} BroConflictPolicy;

/* The wire form ("newFolder"), as in shared/schema/common.json. */
const char *bro_conflict_policy_to_wire (BroConflictPolicy value);
gboolean bro_conflict_policy_from_wire (const char *text, BroConflictPolicy *out);

G_END_DECLS
