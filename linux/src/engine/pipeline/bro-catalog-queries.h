/* bro-catalog-queries.h: BroCatalogQueries: Read-only questions about the archive (plan §12.2), over the catalogue. */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CATALOG_QUERIES (bro_catalog_queries_get_type ())
G_DECLARE_INTERFACE (BroCatalogQueries, bro_catalog_queries, BRO, CATALOG_QUERIES, GObject)

struct _BroCatalogQueriesInterface {
  GTypeInterface parent_iface;
  GPtrArray *(*archived_before) (BroCatalogQueries *self, const char *fingerprint, BroBroError **error);
};

/* Committed units of a disc with this fingerprint (BroUnitRecord *). */
GPtrArray *bro_catalog_queries_archived_before (BroCatalogQueries *self, const char *fingerprint, BroBroError **error);

G_END_DECLS
