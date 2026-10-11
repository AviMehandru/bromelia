/* bro-catalog-queries.c */
#include "bro-catalog-queries.h"

G_DEFINE_INTERFACE (BroCatalogQueries, bro_catalog_queries, G_TYPE_OBJECT)

static void
bro_catalog_queries_default_init (BroCatalogQueriesInterface *iface)
{
}

GPtrArray *
bro_catalog_queries_archived_before (BroCatalogQueries *self, const char *fingerprint, BroBroError **error)
{
  return BRO_CATALOG_QUERIES_GET_IFACE (self)->archived_before (self, fingerprint, error);
}
