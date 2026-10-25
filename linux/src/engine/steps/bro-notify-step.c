/* bro-notify-step.c */
#include "bro-notify-step.h"
#include "bro-steps-private.h"

struct _BroNotifyStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroNotifyStep, bro_notify_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_NOTIFY;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  return bro_resource_set_new ();
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  return bro_step_report_new (NULL);
}

static void
step_iface_init (BroStepInterface *iface)
{
  iface->kind = step_kind;
  iface->resumability = step_resumability;
  iface->resources = step_resources;
  iface->run = step_run;
}

static void
bro_notify_step_class_init (BroNotifyStepClass *klass)
{
}

static void
bro_notify_step_init (BroNotifyStep *self)
{
}

BroNotifyStep *
bro_notify_step_new (void)
{
  return g_object_new (BRO_TYPE_NOTIFY_STEP, NULL);
}
