/* bro-release-step.c */
#include "bro-release-step.h"
#include "bro-steps-private.h"
#include "bro-step-sources.h"
#include "bro-config.h"
struct _BroReleaseStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroReleaseStep, bro_release_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_RELEASE;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  BroResourceSet *set = bro_resource_set_new ();
  const char *drives[] = { job->request->drive_id, NULL };
  set->drives = g_strdupv ((char **) drives);
  return set;
}

static BroStepReport *
ejected (gboolean value)
{
  return bro_step_report_new (_bro_param ("ejected", bro_json_value_new_bool (value)));
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (context->job, error);
  if (!request)
    return NULL;
  if (request->source->kind != BRO_DISC_SOURCE_DRIVE)
    return _bro_step_skipped ();
  g_autoptr (BroDriveLocation) at = context->drive_lookup ? bro_drive_lookup_find (context->drive_lookup, request->source->value) : NULL;
  if (!at || !*at->device)
    return ejected (FALSE);
  g_autoptr (BroJsonValue) automation = bro_config_automation (context->config, at->config_id);
  if (!bro_json_value_get_bool (bro_json_value_member (automation, "ejectWhenDone"), FALSE))
    return ejected (FALSE);
  _bro_step_progress (context, 0, BRO_MSG_DRIVE_EJECTING);
  g_autoptr (BroBroError) failure = NULL;
  if (!bro_drive_control_eject (context->drives, at->device, &failure)) {
    BroJsonValue *params = _bro_param ("device", bro_json_value_new_string (at->device));
    bro_json_value_set (params, "reason", bro_json_value_new_string (failure ? failure->code : ""));
    _bro_step_log (context, BRO_MSG_DRIVE_EJECT_FAILED, params, BRO_SEVERITY_WARNING);
    return ejected (FALSE);
  }
  _bro_step_log (context, BRO_MSG_DRIVE_EJECTED, NULL, BRO_SEVERITY_INFO);
  return ejected (TRUE);
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
bro_release_step_class_init (BroReleaseStepClass *klass)
{
}

static void
bro_release_step_init (BroReleaseStep *self)
{
}

BroReleaseStep *
bro_release_step_new (void)
{
  return g_object_new (BRO_TYPE_RELEASE_STEP, NULL);
}
