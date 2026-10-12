/* bro-await-media-step.c */
#include "bro-await-media-step.h"
#include "bro-steps-private.h"
#include "bro-step-sources.h"
#include "bro-config.h"
struct _BroAwaitMediaStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroAwaitMediaStep, bro_await_media_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_AWAIT_MEDIA;
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
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (context->job, error);
  if (!request)
    return NULL;
  if (request->source->kind != BRO_DISC_SOURCE_DRIVE)
    return _bro_step_skipped ();
  g_autoptr (BroDriveLocation) at = bro_step_sources_drive (context->drive_lookup, request->source->value, error);
  if (!at)
    return NULL;
  const BroJobRequest *r = context->job->request;
  if (r->has_media_generation && (!at->has_generation || at->generation != r->media_generation)) {
    g_autoptr (BroBroMessage) why = bro_bro_message_new (BRO_MSG_DISC_CHANGED_GENERATION, NULL, BRO_SEVERITY_INFO);
    return _bro_step_failed (bro_bro_error_new (bro_message_code_wire (BRO_MSG_DISC_CHANGED), _bro_param ("why", bro_bro_message_to_json (why)), NULL));
  }
  g_autoptr (BroJsonValue) automation = bro_config_automation (context->config, at->config_id);
  gint64 seconds = bro_json_value_get_integer (bro_json_value_member (automation, "waitForMountSeconds"), 0);
  if (seconds <= 0 || !*at->device)
    return _bro_step_skipped ();
  _bro_step_progress (context, 0, BRO_MSG_DISC_WAITING_FOR_MOUNT);
  BroDuration timeout = { (double) seconds };
  g_autofree char *mount = bro_drive_control_wait_for_mount (context->drives, at->device, timeout, context->cancel);
  if (bro_cancellation_token_is_cancelled (context->cancel))
    return _bro_step_failed (bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL, NULL));
  if (mount)
    _bro_step_log (context, BRO_MSG_DISC_MOUNTED, NULL, BRO_SEVERITY_INFO);
  else
    _bro_step_log (context, BRO_MSG_DISC_NOT_MOUNTED, _bro_param ("seconds", bro_json_value_new_integer (seconds)), BRO_SEVERITY_INFO);
  return bro_step_report_new (_bro_param ("mountPath", mount ? bro_json_value_new_string (mount) : bro_json_value_new_null ()));
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
bro_await_media_step_class_init (BroAwaitMediaStepClass *klass)
{
}

static void
bro_await_media_step_init (BroAwaitMediaStep *self)
{
}

BroAwaitMediaStep *
bro_await_media_step_new (void)
{
  return g_object_new (BRO_TYPE_AWAIT_MEDIA_STEP, NULL);
}
