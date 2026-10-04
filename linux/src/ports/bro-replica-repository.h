/* bro-replica-repository.h: BroReplicaRepository: The replicas table. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-replica-record.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_REPLICA_REPOSITORY (bro_replica_repository_get_type ())
G_DECLARE_INTERFACE (BroReplicaRepository, bro_replica_repository, BRO, REPLICA_REPOSITORY, GObject)

struct _BroReplicaRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*upsert) (BroReplicaRepository *self, const BroReplicaRecord *replica, BroBroError **error);
  GPtrArray *(*for_unit) (BroReplicaRepository *self, BroId unit_id, BroBroError **error);
  GPtrArray *(*lagging) (BroReplicaRepository *self, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_replica_repository_upsert (BroReplicaRepository *self, const BroReplicaRecord *replica, BroBroError **error);

GPtrArray *bro_replica_repository_for_unit (BroReplicaRepository *self, BroId unit_id, BroBroError **error);

/* Replicas that aren't verified. */
GPtrArray *bro_replica_repository_lagging (BroReplicaRepository *self, BroBroError **error);

G_END_DECLS
