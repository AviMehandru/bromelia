/* bro-replica-state.h: replicas.state. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_REPLICA_STATE_PENDING,
  BRO_REPLICA_STATE_COPYING,
  BRO_REPLICA_STATE_VERIFIED,
  BRO_REPLICA_STATE_STALE,
  BRO_REPLICA_STATE_FAILED,
} BroReplicaState;

/* The wire form ("pending"). */
const char *bro_replica_state_to_wire (BroReplicaState value);
gboolean bro_replica_state_from_wire (const char *text, BroReplicaState *out);

G_END_DECLS
