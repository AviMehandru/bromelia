/* bro-unit-repository.c */
#include "bro-unit-repository.h"

G_DEFINE_INTERFACE (BroUnitRepository, bro_unit_repository, G_TYPE_OBJECT)

static void
bro_unit_repository_default_init (BroUnitRepositoryInterface *iface)
{
}

gboolean
bro_unit_repository_begin_commit (BroUnitRepository *self, const BroUnitRecord *unit, const BroCommitIntent *intent, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), FALSE);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->begin_commit (self, unit, intent, error);
}

gboolean
bro_unit_repository_mark_moved (BroUnitRepository *self, BroId unit_id, int seq, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), FALSE);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->mark_moved (self, unit_id, seq, error);
}

gboolean
bro_unit_repository_finish_commit (BroUnitRepository *self, const BroCommitIntent *intent, const BroUnitRecord *record, GPtrArray *files, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), FALSE);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->finish_commit (self, intent, record, files, error);
}

GPtrArray *
bro_unit_repository_open_intents (BroUnitRepository *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), NULL);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->open_intents (self, error);
}

BroUnitRecord *
bro_unit_repository_get (BroUnitRepository *self, BroId unit_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), NULL);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->get (self, unit_id, error);
}

GPtrArray *
bro_unit_repository_query (BroUnitRepository *self, const BroUnitQuery *query, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), NULL);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->query (self, query, error);
}

GPtrArray *
bro_unit_repository_least_recently_verified (BroUnitRepository *self, int limit, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), NULL);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->least_recently_verified (self, limit, error);
}

gboolean
bro_unit_repository_mark_missing (BroUnitRepository *self, BroId unit_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_UNIT_REPOSITORY (self), FALSE);
  return BRO_UNIT_REPOSITORY_GET_IFACE (self)->mark_missing (self, unit_id, error);
}
