/* bro-job-repository.c */
#include "bro-job-repository.h"

G_DEFINE_INTERFACE (BroJobRepository, bro_job_repository, G_TYPE_OBJECT)

static void
bro_job_repository_default_init (BroJobRepositoryInterface *iface)
{
}

gboolean
bro_job_repository_insert (BroJobRepository *self, const BroJobRecord *job, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_JOB_REPOSITORY (self), FALSE);
  return BRO_JOB_REPOSITORY_GET_IFACE (self)->insert (self, job, error);
}

gboolean
bro_job_repository_update (BroJobRepository *self, const BroJobRecord *job, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_JOB_REPOSITORY (self), FALSE);
  return BRO_JOB_REPOSITORY_GET_IFACE (self)->update (self, job, error);
}

BroJobRecord *
bro_job_repository_load (BroJobRepository *self, BroId job_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_JOB_REPOSITORY (self), NULL);
  return BRO_JOB_REPOSITORY_GET_IFACE (self)->load (self, job_id, error);
}

GPtrArray *
bro_job_repository_active (BroJobRepository *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_JOB_REPOSITORY (self), NULL);
  return BRO_JOB_REPOSITORY_GET_IFACE (self)->active (self, error);
}

GPtrArray *
bro_job_repository_query (BroJobRepository *self, const BroJobQuery *query, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_JOB_REPOSITORY (self), NULL);
  return BRO_JOB_REPOSITORY_GET_IFACE (self)->query (self, query, error);
}
