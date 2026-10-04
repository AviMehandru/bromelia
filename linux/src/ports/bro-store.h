/* bro-store.h: BroStore: Repositories over shared/schema/db; every write goes through one writer. */
#pragma once

#include "bro-bro-error.h"
#include "bro-catalog-repository.h"
#include "bro-check-repository.h"
#include "bro-drive-repository.h"
#include "bro-job-repository.h"
#include "bro-key-value-repository.h"
#include "bro-lookup-cache-repository.h"
#include "bro-outbox-repository.h"
#include "bro-replica-repository.h"
#include "bro-step-repository.h"
#include "bro-unit-repository.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_STORE (bro_store_get_type ())
G_DECLARE_INTERFACE (BroStore, bro_store, BRO, STORE, GObject)

/* The body of a transaction; FALSE (with @error set) rolls it back. */
typedef gboolean (*BroStoreBlockFunc) (BroStore *store, gpointer data, BroBroError **error);

struct _BroStoreInterface {
  GTypeInterface parent_iface;

  BroJobRepository *(*jobs) (BroStore *self);
  BroStepRepository *(*steps) (BroStore *self);
  BroUnitRepository *(*units) (BroStore *self);
  BroCatalogRepository *(*catalog) (BroStore *self);
  BroCheckRepository *(*checks) (BroStore *self);
  BroReplicaRepository *(*replicas) (BroStore *self);
  BroDriveRepository *(*drives) (BroStore *self);
  BroLookupCacheRepository *(*lookup_cache) (BroStore *self);
  BroOutboxRepository *(*outbox) (BroStore *self);
  BroKeyValueRepository *(*kv) (BroStore *self);
  gboolean (*transaction) (BroStore *self, BroStoreBlockFunc block, gpointer data, BroBroError **error);
  gboolean (*migrate) (BroStore *self, BroBroError **error);
};

BroJobRepository *bro_store_jobs (BroStore *self);

BroStepRepository *bro_store_steps (BroStore *self);

BroUnitRepository *bro_store_units (BroStore *self);

BroCatalogRepository *bro_store_catalog (BroStore *self);

BroCheckRepository *bro_store_checks (BroStore *self);

BroReplicaRepository *bro_store_replicas (BroStore *self);

BroDriveRepository *bro_store_drives (BroStore *self);

BroLookupCacheRepository *bro_store_lookup_cache (BroStore *self);

BroOutboxRepository *bro_store_outbox (BroStore *self);

BroKeyValueRepository *bro_store_kv (BroStore *self);

/* Runs block in one transaction; a failure rolls it back. FALSE and @error set on failure. */
gboolean bro_store_transaction (BroStore *self, BroStoreBlockFunc block, gpointer data, BroBroError **error);

/* Applies the migrations the database lacks (shared/schema/db). FALSE and @error set on failure. */
gboolean bro_store_migrate (BroStore *self, BroBroError **error);

G_END_DECLS
