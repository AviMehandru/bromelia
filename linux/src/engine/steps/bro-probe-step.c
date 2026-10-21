/* bro-probe-step.c */
#include "bro-probe-step.h"
#include "bro-steps-private.h"
#include "bro-step-sources.h"
#include "bro-fingerprint.h"
#include "bro-makemkv-sink.h"
#include "bro-probe-output.h"
struct _BroProbeStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroProbeStep, bro_probe_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_PROBE;
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
  g_autoptr (BroDiscRequest) request = bro_disc_request_decode (job->request);
  const char *drives[] = { request && request->source->kind == BRO_DISC_SOURCE_DRIVE ? request->source->value : NULL, NULL };
  set->drives = g_strdupv ((char **) drives);
  return set;
}

/* The source as makemkvcon names it: disc:N, iso:path or file:path. */
static char *
describe (const BroMakemkvSource *source)
{
  switch (source->kind) {
  case BRO_MAKEMKV_SOURCE_DRIVE:
    return g_strdup_printf ("disc:%d", source->index);
  case BRO_MAKEMKV_SOURCE_ISO:
    return g_strconcat ("iso:", source->path, NULL);
  case BRO_MAKEMKV_SOURCE_FILE:
    return g_strconcat ("file:", source->path, NULL);
  }
  return g_strdup ("");
}

static BroStepReport *
done (BroListing *listing, const BroMakemkvRun *run, gboolean from_session)
{
  g_autofree char *fingerprint = listing ? bro_fingerprint_of (listing) : NULL;
  g_autoptr (BroProbeOutput) output = bro_probe_output_new (listing, fingerprint, run->libre_drive, from_session);
  output->makemkv_version = g_strdup (run->version);
  return bro_step_report_new (bro_probe_output_encode (output));
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (context->job, error);
  if (!request)
    return NULL;
  g_autoptr (BroMakemkvSource) source = bro_step_sources_makemkv (request->source, context->drive_lookup, error);
  if (!source)
    return NULL;
  g_autoptr (BroDriveLocation) at = request->source->kind == BRO_DISC_SOURCE_DRIVE && context->drive_lookup
                                        ? bro_drive_lookup_find (context->drive_lookup, request->source->value)
                                        : NULL;
  g_autoptr (BroEffectiveProfile) profile = bro_step_sources_profile (context, at ? at->config_id : NULL);
  g_autoptr (BroBroMessage) operation = bro_bro_message_new (BRO_MSG_DISC_OP_READING, NULL, BRO_SEVERITY_INFO);
  if (request->session)
    _bro_step_log (context, BRO_MSG_DISC_READING_AGAIN, NULL, BRO_SEVERITY_INFO);
  bro_progress_reporter_report (context->progress, 0, operation, NULL);
  g_autoptr (BroMakemkvInvocation) invocation = bro_makemkv_setup_invocation (context->makemkv_setup, context->config, profile, NULL, context->job_directory, error);
  if (!invocation)
    return NULL;
  g_autoptr (BroMakemkvSink) sink = bro_makemkv_sink_new (context->progress, context->log, operation);
  g_autoptr (BroListingRun) read = bro_makemkv_listing (context->tools->makemkv, source, invocation, BRO_RUN_SINK (sink), context->cancel, error);
  if (!read)
    return NULL;
  const BroMakemkvRun *run = read->run;
  const BroBroMessage *problems[] = { run->isolation_warning, run->transcript_problem, run->settings_problem };
  for (guint i = 0; i < G_N_ELEMENTS (problems); i++)
    if (problems[i]) {
      g_autoptr (BroBroMessage) warning = bro_bro_message_copy (problems[i]);
      warning->severity = BRO_SEVERITY_WARNING;
      bro_job_logger_log (context->log, warning);
    }
  if (run->outcome->status == BRO_STATUS_WORD_CANCELLED || bro_cancellation_token_is_cancelled (context->cancel))
    return _bro_step_failed (bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL, NULL));
  if (run->libre_drive)
    _bro_step_log (context, BRO_MSG_MAKEMKV_LIBRE_DRIVE, _bro_param ("detail", bro_json_value_new_string (run->libre_drive)), BRO_SEVERITY_INFO);

  BroListing *listing = read->listing;
  if (listing->titles->len == 0) {
    g_autoptr (BroBroMessage) exit_status = NULL;
    const BroBroMessage *reason = run->outcome->error;
    if (!reason)
      reason = exit_status = bro_bro_message_new (BRO_MSG_PROCESS_EXIT_STATUS, _bro_param ("status", bro_json_value_new_integer (run->outcome->exit_code)),
                                                  BRO_SEVERITY_ERROR);
    g_autofree char *described = describe (source);
    BroJsonValue *params = _bro_param ("source", bro_json_value_new_string (described));
    bro_json_value_set (params, "reason", bro_bro_message_to_json (reason));
    g_autoptr (BroBroMessage) no_titles = bro_bro_message_new (BRO_MSG_DISC_NO_TITLES, params, BRO_SEVERITY_ERROR);
    BroRipMode mode = bro_step_sources_base_mode (context->job, profile);
    if (mode != BRO_RIP_MODE_BACKUP && mode != BRO_RIP_MODE_BACKUP_DECRYPTED) {
      BroStepReport *report = _bro_step_failed (bro_bro_message_to_error (no_titles, NULL));
      report->notice = run->notice ? bro_makemkv_notice_copy (run->notice) : NULL;
      return report;
    }
    if (request->session) {
      _bro_step_log (context, BRO_MSG_DISC_USING_OPENED_LISTING, _bro_param ("reason", bro_bro_message_to_json (no_titles)), BRO_SEVERITY_WARNING);
      return done (request->session->listing, run, TRUE);
    }
    _bro_step_log (context, BRO_MSG_DISC_CONTINUING_WITHOUT_LISTING, _bro_param ("reason", bro_bro_message_to_json (no_titles)), BRO_SEVERITY_WARNING);
    return done (NULL, run, FALSE);
  }
  BroJsonValue *found = _bro_param ("count", bro_json_value_new_integer (listing->titles->len));
  bro_json_value_set (found, "name", bro_json_value_new_string (listing->name));
  _bro_step_log (context, BRO_MSG_DISC_FOUND, found, BRO_SEVERITY_INFO);
  return done (listing, run, FALSE);
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
bro_probe_step_class_init (BroProbeStepClass *klass)
{
}

static void
bro_probe_step_init (BroProbeStep *self)
{
}

BroProbeStep *
bro_probe_step_new (void)
{
  return g_object_new (BRO_TYPE_PROBE_STEP, NULL);
}
