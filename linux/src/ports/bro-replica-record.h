/* bro-replica-record.h: A row of replicas: a verified second copy. */
#pragma once

#include "bro-bro-message.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-replica-state.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  BroId unit_id;
  char *target_id;
  char *path;
  BroReplicaState state;
  gboolean has_verified_at;
  BroInstant verified_at;
  BroBroMessage *error; /* nullable */
} BroReplicaRecord;

/* Everything zero. */
BroReplicaRecord *bro_replica_record_new (void);
void bro_replica_record_free (BroReplicaRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroReplicaRecord, bro_replica_record_free)

G_END_DECLS
