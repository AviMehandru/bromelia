/* bro-step-repository.c */
#include "bro-step-repository.h"

G_DEFINE_INTERFACE (BroStepRepository, bro_step_repository, G_TYPE_OBJECT)

static void
bro_step_repository_default_init (BroStepRepositoryInterface *iface)
{
}

gboolean
bro_step_repository_save (BroStepRepository *self, const BroStepRecord *step, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_STEP_REPOSITORY (self), FALSE);
  return BRO_STEP_REPOSITORY_GET_IFACE (self)->save (self, step, error);
}

GPtrArray *
bro_step_repository_completed (BroStepRepository *self, BroId job_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_STEP_REPOSITORY (self), NULL);
  return BRO_STEP_REPOSITORY_GET_IFACE (self)->completed (self, job_id, error);
}

GPtrArray *
bro_step_repository_load (BroStepRepository *self, BroId job_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_STEP_REPOSITORY (self), NULL);
  return BRO_STEP_REPOSITORY_GET_IFACE (self)->load (self, job_id, error);
}
