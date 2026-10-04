/* bro-catalog-repository.h: BroCatalogRepository: What has been archived. */
#pragma once

#include "bro-archived-disc.h"
#include "bro-bro-error.h"
#include "bro-continuation-query.h"
#include "bro-disc-set-record.h"
#include "bro-id.h"
#include "bro-unit-record.h"
#include "bro-work-record.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CATALOG_REPOSITORY (bro_catalog_repository_get_type ())
G_DECLARE_INTERFACE (BroCatalogRepository, bro_catalog_repository, BRO, CATALOG_REPOSITORY, GObject)

struct _BroCatalogRepositoryInterface {
  GTypeInterface parent_iface;

  GPtrArray *(*archived_before) (BroCatalogRepository *self, const char *fingerprint, BroBroError **error);
  BroArchivedDisc *(*previous_disc) (BroCatalogRepository *self, const BroContinuationQuery *query, BroBroError **error);
  int (*highest_episode) (BroCatalogRepository *self, const BroContinuationQuery *query, BroBroError **error);
  GPtrArray *(*works) (BroCatalogRepository *self, const char *query, BroBroError **error);
  BroDiscSetRecord *(*set) (BroCatalogRepository *self, BroId set_id, BroBroError **error);
};

/* Committed units of the same disc. */
GPtrArray *bro_catalog_repository_archived_before (BroCatalogRepository *self, const char *fingerprint, BroBroError **error);

/* The disc before this one of the same set. NULL when there is none. */
BroArchivedDisc *bro_catalog_repository_previous_disc (BroCatalogRepository *self, const BroContinuationQuery *query, BroBroError **error);

/* -1 when there is none (or on failure, with @error set). */
int bro_catalog_repository_highest_episode (BroCatalogRepository *self, const BroContinuationQuery *query, BroBroError **error);

/* Works whose title contains query, ignoring case. */
GPtrArray *bro_catalog_repository_works (BroCatalogRepository *self, const char *query, BroBroError **error);

/* NULL when there is none. */
BroDiscSetRecord *bro_catalog_repository_set (BroCatalogRepository *self, BroId set_id, BroBroError **error);

G_END_DECLS
