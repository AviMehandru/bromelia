/* bro-check-repository.c */
#include "bro-check-repository.h"

G_DEFINE_INTERFACE (BroCheckRepository, bro_check_repository, G_TYPE_OBJECT)

static void
bro_check_repository_default_init (BroCheckRepositoryInterface *iface)
{
}

gboolean
bro_check_repository_insert (BroCheckRepository *self, const BroCheckRecord *check, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CHECK_REPOSITORY (self), FALSE);
  return BRO_CHECK_REPOSITORY_GET_IFACE (self)->insert (self, check, error);
}

GPtrArray *
bro_check_repository_for_unit (BroCheckRepository *self, BroId unit_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CHECK_REPOSITORY (self), NULL);
  return BRO_CHECK_REPOSITORY_GET_IFACE (self)->for_unit (self, unit_id, error);
}

BroCheckRecord *
bro_check_repository_latest (BroCheckRepository *self, const char *folder, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CHECK_REPOSITORY (self), NULL);
  return BRO_CHECK_REPOSITORY_GET_IFACE (self)->latest (self, folder, error);
}
