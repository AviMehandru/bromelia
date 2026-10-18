/* bro-decide-step.c */
#include "bro-decide-step.h"
#include "bro-archive-check.h"
#include "bro-decision-dto.h"
#include "bro-plan-output.h"
#include "bro-probe-output.h"
#include "bro-space-check.h"
#include "bro-step-sources.h"
#include "bro-steps-private.h"

#include <string.h>

struct _BroDecideStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroDecideStep, bro_decide_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_DECIDE;
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
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  const char *writes[] = { plan && *plan->library_id ? plan->library_id : NULL, NULL };
  set->drives = g_new0 (char *, 1);
  set->library_writes = g_strdupv ((char **) writes);
  return set;
}

/* {answer} (@answer nullable); takes @skip. */
static BroStepReport *
answered (const char *answer, BroBroMessage *skip)
{
  BroStepReport *r = bro_step_report_new (_bro_param ("answer", answer ? bro_json_value_new_string (answer) : bro_json_value_new_null ()));
  r->skip_job = skip;
  return r;
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (context->job);
  g_autoptr (BroDiscRequest) request = NULL;
  g_autoptr (BroDriveLocation) drive = NULL;
  g_autoptr (BroArchivedMatch) found = NULL;
  const BroArchivedMatch *ask;
  g_autofree char *answer = NULL;
  if (!plan)
    {
      BroJsonValue *params = _bro_param ("detail", bro_json_value_new_string ("decide without a plan"));
      *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), params, NULL);
      return NULL;
    }
  request = bro_step_sources_request (context->job, error);
  if (!request)
    return NULL;
  if (request->source->kind == BRO_DISC_SOURCE_DRIVE && context->drive_lookup)
    drive = bro_drive_lookup_find (context->drive_lookup, request->source->value);
  ask = plan->ask_archived;
  if (plan->library_offline && plan->mode != BRO_RIP_MODE_INFO_ONLY)
    {
      g_autoptr (BroProbeOutput) probe = bro_probe_output_decode (bro_job_snapshot_output (context->job, BRO_STEP_KIND_PROBE));
      g_autoptr (BroBroError) failure = NULL;
      found = bro_archive_check_find (context, probe->fingerprint, &failure);
      if (failure)
        {
          *error = g_steal_pointer (&failure);
          return NULL;
        }
      if (found)
        {
          BroBroMessage *skip = bro_archive_check_apply (context, found, drive ? drive->config_id : NULL);
          if (skip)
            return answered (NULL, skip);
          if (bro_archive_check_asks (context, drive ? drive->config_id : NULL))
            ask = found;
        }
    }
  if (ask)
    {
      g_autoptr (BroBroMessage) question = bro_archive_check_message (BRO_MSG_DECISION_ARCHIVED_BEFORE, ask, FALSE);
      g_autoptr (BroDecisionDto) decision = NULL;
      const struct {
        const char *id;
        BroMessageCode label;
      } answers[] = { { "ripAgain", BRO_MSG_DECISION_ANSWER_RIP_AGAIN }, { "skip", BRO_MSG_DECISION_ANSWER_SKIP }, { "eject", BRO_MSG_DECISION_ANSWER_EJECT } };
      question->severity = BRO_SEVERITY_INFO;
      decision = bro_decision_dto_new (BRO_DECIDE_STEP_ARCHIVED_BEFORE, bro_bro_message_copy (question));
      for (guint i = 0; i < G_N_ELEMENTS (answers); i++)
        g_ptr_array_add (decision->answers, bro_decision_answer_new (answers[i].id, bro_bro_message_new (answers[i].label, NULL, BRO_SEVERITY_INFO)));
      answer = bro_decision_port_ask (context->decisions, decision, error);
      if (!answer)
        return NULL;
      if (strcmp (answer, "ripAgain") != 0)
        return answered (answer, bro_archive_check_message (BRO_MSG_DISC_ALREADY_ARCHIVED_ASK, ask, TRUE));
      {
        g_autoptr (BroBroMessage) line = bro_archive_check_message (BRO_MSG_DISC_ARCHIVED_BEFORE, ask, FALSE);
        bro_job_logger_log (context->log, line);
      }
    }
  if (!plan->space_reserved && plan->has_space_need)
    {
      g_autoptr (BroLibraryHandle) library = bro_library_port_require (context->library, plan->library_id, error);
      if (!library || !bro_space_check_reserve (context, library->path, plan->space_need, error))
        return NULL;
    }
  return answered (answer, NULL);
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
bro_decide_step_class_init (BroDecideStepClass *klass)
{
}

static void
bro_decide_step_init (BroDecideStep *self)
{
}

BroDecideStep *
bro_decide_step_new (void)
{
  return g_object_new (BRO_TYPE_DECIDE_STEP, NULL);
}
