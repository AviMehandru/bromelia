/* bro-check-repository.h: BroCheckRepository: The checks table. */
#pragma once

#include "bro-bro-error.h"
#include "bro-check-record.h"
#include "bro-id.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CHECK_REPOSITORY (bro_check_repository_get_type ())
G_DECLARE_INTERFACE (BroCheckRepository, bro_check_repository, BRO, CHECK_REPOSITORY, GObject)

struct _BroCheckRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*insert) (BroCheckRepository *self, const BroCheckRecord *check, BroBroError **error);
  GPtrArray *(*for_unit) (BroCheckRepository *self, BroId unit_id, BroBroError **error);
  BroCheckRecord *(*latest) (BroCheckRepository *self, const char *folder, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_check_repository_insert (BroCheckRepository *self, const BroCheckRecord *check, BroBroError **error);

/* Newest first. */
GPtrArray *bro_check_repository_for_unit (BroCheckRepository *self, BroId unit_id, BroBroError **error);

/* NULL when there is none. */
BroCheckRecord *bro_check_repository_latest (BroCheckRepository *self, const char *folder, BroBroError **error);

G_END_DECLS
