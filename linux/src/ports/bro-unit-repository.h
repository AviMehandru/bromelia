/* bro-unit-repository.h: BroUnitRepository: archive_units, unit_files and the commit intents. */
#pragma once

#include "bro-bro-error.h"
#include "bro-commit-intent.h"
#include "bro-id.h"
#include "bro-unit-file.h"
#include "bro-unit-query.h"
#include "bro-unit-record.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_UNIT_REPOSITORY (bro_unit_repository_get_type ())
G_DECLARE_INTERFACE (BroUnitRepository, bro_unit_repository, BRO, UNIT_REPOSITORY, GObject)

struct _BroUnitRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*begin_commit) (BroUnitRepository *self, const BroUnitRecord *unit, const BroCommitIntent *intent, BroBroError **error);
  gboolean (*mark_moved) (BroUnitRepository *self, BroId unit_id, int seq, BroBroError **error);
  gboolean (*finish_commit) (BroUnitRepository *self, const BroCommitIntent *intent, const BroUnitRecord *record, GPtrArray *files, BroBroError **error);
  GPtrArray *(*open_intents) (BroUnitRepository *self, BroBroError **error);
  BroUnitRecord *(*get) (BroUnitRepository *self, BroId unit_id, BroBroError **error);
  GPtrArray *(*query) (BroUnitRepository *self, const BroUnitQuery *query, BroBroError **error);
  GPtrArray *(*least_recently_verified) (BroUnitRepository *self, int limit, BroBroError **error);
  gboolean (*mark_missing) (BroUnitRepository *self, BroId unit_id, BroBroError **error);
};

/* The unit (committing) and its intent, in one transaction. FALSE and @error set on failure. */
gboolean bro_unit_repository_begin_commit (BroUnitRepository *self, const BroUnitRecord *unit, const BroCommitIntent *intent, BroBroError **error);

/* Item seq of the intent has been moved. FALSE and @error set on failure. */
gboolean bro_unit_repository_mark_moved (BroUnitRepository *self, BroId unit_id, int seq, BroBroError **error);

/* The committed unit and its files; the intent goes. FALSE and @error set on failure. */
gboolean bro_unit_repository_finish_commit (BroUnitRepository *self, const BroCommitIntent *intent, const BroUnitRecord *record, GPtrArray *files, BroBroError **error);

GPtrArray *bro_unit_repository_open_intents (BroUnitRepository *self, BroBroError **error);

/* NULL when there is none. */
BroUnitRecord *bro_unit_repository_get (BroUnitRepository *self, BroId unit_id, BroBroError **error);

GPtrArray *bro_unit_repository_query (BroUnitRepository *self, const BroUnitQuery *query, BroBroError **error);

/* Committed units, the ones checked longest ago first. */
GPtrArray *bro_unit_repository_least_recently_verified (BroUnitRepository *self, int limit, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_unit_repository_mark_missing (BroUnitRepository *self, BroId unit_id, BroBroError **error);

G_END_DECLS
