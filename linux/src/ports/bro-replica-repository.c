/* bro-replica-repository.c */
#include "bro-replica-repository.h"

G_DEFINE_INTERFACE (BroReplicaRepository, bro_replica_repository, G_TYPE_OBJECT)

static void
bro_replica_repository_default_init (BroReplicaRepositoryInterface *iface)
{
}

gboolean
bro_replica_repository_upsert (BroReplicaRepository *self, const BroReplicaRecord *replica, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_REPLICA_REPOSITORY (self), FALSE);
  return BRO_REPLICA_REPOSITORY_GET_IFACE (self)->upsert (self, replica, error);
}

GPtrArray *
bro_replica_repository_for_unit (BroReplicaRepository *self, BroId unit_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_REPLICA_REPOSITORY (self), NULL);
  return BRO_REPLICA_REPOSITORY_GET_IFACE (self)->for_unit (self, unit_id, error);
}

GPtrArray *
bro_replica_repository_lagging (BroReplicaRepository *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_REPLICA_REPOSITORY (self), NULL);
  return BRO_REPLICA_REPOSITORY_GET_IFACE (self)->lagging (self, error);
}
