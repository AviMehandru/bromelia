/* bro-release-step.c */
#include "bro-release-step.h"
#include "bro-steps-private.h"
#include "bro-step-sources.h"
#include "bro-config.h"

#include <string.h>
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

/* At the end of a job that stopped before it (the context's finishing, owner 2026-10-18): after a failure when
 * automation.ejectOnFailure says so, after "already archived, skipped" (disc.alreadyArchived) when ejectWhenDone says so,
 * and when the user answered eject to Decide's question; never after a cancel. Otherwise ejectWhenDone. */
static gboolean
ejects (const BroStepContext *context, const BroJsonValue *automation)
{
  const BroDecided *end = context->finishing;
  BroJsonValue *decide;
  if (!end)
    return bro_json_value_get_bool (bro_json_value_member (automation, "ejectWhenDone"), FALSE);
  switch (end->outcome)
    {
    case BRO_OUTCOME_FAILED:
      return bro_json_value_get_bool (bro_json_value_member (automation, "ejectOnFailure"), FALSE);
    case BRO_OUTCOME_SKIPPED:
      decide = bro_job_snapshot_output (context->job, BRO_STEP_KIND_DECIDE);
      if (g_strcmp0 (bro_json_value_get_string (bro_json_value_member (decide, "answer"), NULL), "eject") == 0)
        return TRUE;
      return end->error && strcmp (end->error->code, bro_message_code_wire (BRO_MSG_DISC_ALREADY_ARCHIVED)) == 0
             && bro_json_value_get_bool (bro_json_value_member (automation, "ejectWhenDone"), FALSE);
    default:
      return FALSE;
    }
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
  if (!ejects (context, automation))
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
