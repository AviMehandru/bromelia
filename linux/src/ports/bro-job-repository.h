/* bro-job-repository.h: BroJobRepository: The jobs table. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-job-query.h"
#include "bro-job-record.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_JOB_REPOSITORY (bro_job_repository_get_type ())
G_DECLARE_INTERFACE (BroJobRepository, bro_job_repository, BRO, JOB_REPOSITORY, GObject)

struct _BroJobRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*insert) (BroJobRepository *self, const BroJobRecord *job, BroBroError **error);
  gboolean (*update) (BroJobRepository *self, const BroJobRecord *job, BroBroError **error);
  BroJobRecord *(*load) (BroJobRepository *self, BroId job_id, BroBroError **error);
  GPtrArray *(*active) (BroJobRepository *self, BroBroError **error);
  GPtrArray *(*query) (BroJobRepository *self, const BroJobQuery *query, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_job_repository_insert (BroJobRepository *self, const BroJobRecord *job, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_job_repository_update (BroJobRepository *self, const BroJobRecord *job, BroBroError **error);

/* NULL when there is none. */
BroJobRecord *bro_job_repository_load (BroJobRepository *self, BroId job_id, BroBroError **error);

/* Jobs that haven't finished, by queue and position. */
GPtrArray *bro_job_repository_active (BroJobRepository *self, BroBroError **error);

GPtrArray *bro_job_repository_query (BroJobRepository *self, const BroJobQuery *query, BroBroError **error);

G_END_DECLS
