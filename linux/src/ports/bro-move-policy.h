/* bro-move-policy.h: what moveMerging does when the destination exists: never replace (ConflictNamer picks another
 * name). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_MOVE_POLICY_NEVER_REPLACE,
} BroMovePolicy;

/* The wire form ("neverReplace"). */
const char *bro_move_policy_to_wire (BroMovePolicy value);
gboolean bro_move_policy_from_wire (const char *text, BroMovePolicy *out);

G_END_DECLS
