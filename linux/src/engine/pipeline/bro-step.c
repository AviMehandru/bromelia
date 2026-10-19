/* bro-step.c */
#include "bro-step.h"

G_DEFINE_INTERFACE (BroStep, bro_step, G_TYPE_OBJECT)

static void
bro_step_default_init (BroStepInterface *iface)
{
}

BroStepKind
bro_step_kind (BroStep *self)
{
  return BRO_STEP_GET_IFACE (self)->kind (self);
}

BroResumability
bro_step_resumability (BroStep *self)
{
  return BRO_STEP_GET_IFACE (self)->resumability (self);
}

const char *
bro_step_resume_checkpoint (BroStep *self)
{
  BroStepInterface *iface = BRO_STEP_GET_IFACE (self);
  return iface->resume_checkpoint ? iface->resume_checkpoint (self) : NULL;
}

BroResourceSet *
bro_step_resources (BroStep *self, const BroJobSnapshot *job)
{
  return BRO_STEP_GET_IFACE (self)->resources (self, job);
}

BroStepReport *
bro_step_run (BroStep *self, const BroStepContext *context, BroBroError **error)
{
  return BRO_STEP_GET_IFACE (self)->run (self, context, error);
}
