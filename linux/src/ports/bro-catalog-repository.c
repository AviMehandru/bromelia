/* bro-catalog-repository.c */
#include "bro-catalog-repository.h"

G_DEFINE_INTERFACE (BroCatalogRepository, bro_catalog_repository, G_TYPE_OBJECT)

static void
bro_catalog_repository_default_init (BroCatalogRepositoryInterface *iface)
{
}

GPtrArray *
bro_catalog_repository_archived_before (BroCatalogRepository *self, const char *fingerprint, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CATALOG_REPOSITORY (self), NULL);
  return BRO_CATALOG_REPOSITORY_GET_IFACE (self)->archived_before (self, fingerprint, error);
}

BroArchivedDisc *
bro_catalog_repository_previous_disc (BroCatalogRepository *self, const BroContinuationQuery *query, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CATALOG_REPOSITORY (self), NULL);
  return BRO_CATALOG_REPOSITORY_GET_IFACE (self)->previous_disc (self, query, error);
}

int
bro_catalog_repository_highest_episode (BroCatalogRepository *self, const BroContinuationQuery *query, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CATALOG_REPOSITORY (self), -1);
  return BRO_CATALOG_REPOSITORY_GET_IFACE (self)->highest_episode (self, query, error);
}

GPtrArray *
bro_catalog_repository_works (BroCatalogRepository *self, const char *query, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CATALOG_REPOSITORY (self), NULL);
  return BRO_CATALOG_REPOSITORY_GET_IFACE (self)->works (self, query, error);
}

BroDiscSetRecord *
bro_catalog_repository_set (BroCatalogRepository *self, BroId set_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_CATALOG_REPOSITORY (self), NULL);
  return BRO_CATALOG_REPOSITORY_GET_IFACE (self)->set (self, set_id, error);
}
