/* bro-identify-step.c */
#include "bro-identify-step.h"
#include "bro-steps-private.h"
#include "bro-step-sources.h"
#include "bro-identity.h"
#include "bro-label-parser.h"
#include "bro-probe-output.h"
struct _BroIdentifyStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroIdentifyStep, bro_identify_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_IDENTIFY;
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
  set->drives = g_new0 (char *, 1);
  return set;
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (context->job, error);
  if (!request)
    return NULL;
  g_autoptr (BroProbeOutput) probe = bro_probe_output_decode (bro_job_snapshot_output (context->job, BRO_STEP_KIND_PROBE));
  g_autoptr (BroDriveLocation) drive = request->source->kind == BRO_DISC_SOURCE_DRIVE && context->drive_lookup
                                           ? bro_drive_lookup_find (context->drive_lookup, request->source->value)
                                           : NULL;
  g_autofree char *label = probe->listing && *probe->listing->name ? g_strdup (probe->listing->name)
                                                                    : bro_step_sources_label (request->source, drive);
  g_autoptr (BroEffectiveProfile) profile = bro_step_sources_profile (context, drive ? drive->config_id : NULL);
  BroRipMode mode = bro_step_sources_base_mode (context->job, profile);
  const BroSessionChoices *choices = request->session ? request->session->choices : NULL;
  g_autoptr (BroIdentityInputs) inputs = bro_identity_inputs_new (probe->listing, label, mode == BRO_RIP_MODE_BACKUP);
  if (drive && drive->has_flags) {
    inputs->has_flags = TRUE;
    inputs->flags = drive->flags;
  }
  if (choices) {
    g_free (inputs->name_override);
    inputs->name_override = g_strdup (choices->name);
    inputs->has_kind_override = choices->has_kind;
    inputs->kind_override = choices->kind;
  }
  g_autoptr (BroIdentity) identity = bro_identity_resolve (inputs);
  g_autofree char *set = bro_label_parser_set_description (identity->label);
  BroJsonValue *params = _bro_param ("kind", bro_json_value_new_string (bro_media_kind_to_wire (identity->kind)));
  bro_json_value_set (params, "name", bro_json_value_new_string (identity->name));
  bro_json_value_set (params, "hasSet", bro_json_value_new_bool (*set != '\0'));
  bro_json_value_set (params, "set", bro_json_value_new_string (set));
  bro_json_value_set (params, "format", bro_json_value_new_string (bro_disc_format_to_wire (identity->format)));
  bro_json_value_set (params, "formatCode", bro_json_value_new_string (identity->format_code.text));
  bro_json_value_set (params, "reason", bro_bro_message_to_json (identity->reason));
  _bro_step_log (context, BRO_MSG_IDENTITY_RESOLVED, params, BRO_SEVERITY_INFO);
  return bro_step_report_new (bro_identity_encode (identity));
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
bro_identify_step_class_init (BroIdentifyStepClass *klass)
{
}

static void
bro_identify_step_init (BroIdentifyStep *self)
{
}

BroIdentifyStep *
bro_identify_step_new (void)
{
  return g_object_new (BRO_TYPE_IDENTIFY_STEP, NULL);
}
