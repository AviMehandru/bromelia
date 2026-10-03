/* bro-job-outcome.c */
#include "bro-job-outcome.h"

/* Steps after the archive is committed: their failures never fail a job. */
static gboolean
after_commit (BroStepKind k)
{
  return k == BRO_STEP_KIND_PUBLISH || k == BRO_STEP_KIND_PROTECT || k == BRO_STEP_KIND_POST_PROCESS
         || k == BRO_STEP_KIND_NOTIFY || k == BRO_STEP_KIND_RUN_COMMAND || k == BRO_STEP_KIND_TRANSCODE;
}

static BroDecided *
decided (BroOutcome outcome, BroBroError *error)
{
  BroDecided *d = g_new0 (BroDecided, 1);
  d->outcome = outcome;
  d->error = error;
  return d;
}

static BroBroError *
error_of (BroMessageCode code, BroSeverity severity, BroJsonValue *params, BroBroError *cause)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, severity);
  return bro_bro_message_to_error (m, cause);
}

static gboolean
problem_code (const BroMakemkvNotice *n, BroMessageCode *out)
{
  switch (n->kind) {
  case BRO_MAKEMKV_NOTICE_KEY_EXPIRED: *out = BRO_MSG_MAKEMKV_KEY_EXPIRED; return TRUE;
  case BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED: *out = BRO_MSG_MAKEMKV_EVALUATION_NOT_STARTED; return TRUE;
  case BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD: *out = BRO_MSG_MAKEMKV_TOO_OLD; return TRUE;
  case BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED: *out = BRO_MSG_MAKEMKV_LIBRE_DRIVE_REQUIRED; return TRUE;
  default: return FALSE;
  }
}

BroDecided *
bro_job_outcome_decide (GPtrArray *steps, gboolean cancelled, const BroOutcomePolicy *policy)
{
  if (cancelled)
    return decided (BRO_OUTCOME_CANCELLED, error_of (BRO_MSG_JOB_CANCELLED_BY_USER, BRO_SEVERITY_WARNING, NULL, NULL));
  for (guint i = 0; i < steps->len; i++) {
    const BroStepResult *s = steps->pdata[i];
    if (s->state == BRO_STEP_STATE_INTERRUPTED) {
      BroJsonValue *p = bro_json_value_new_object ();
      bro_json_value_set (p, "step", bro_json_value_new_string (bro_step_kind_to_wire (s->kind)));
      return decided (BRO_OUTCOME_INTERRUPTED, error_of (BRO_MSG_RECOVERY_INTERRUPTED, BRO_SEVERITY_WARNING, p, NULL));
    }
  }
  for (guint i = 0; i < steps->len; i++) {
    const BroStepResult *s = steps->pdata[i];
    if (s->skip)
      return decided (BRO_OUTCOME_SKIPPED, bro_bro_message_to_error (s->skip, NULL));
  }
  for (guint i = 0; i < steps->len; i++) {
    const BroStepResult *s = steps->pdata[i];
    if (s->state != BRO_STEP_STATE_FAILED || after_commit (s->kind))
      continue;
    BroBroError *error = s->error ? bro_bro_error_copy (s->error)
                                  : error_of (BRO_MSG_INTERNAL_UNEXPECTED, BRO_SEVERITY_ERROR, NULL, NULL);
    BroMessageCode problem;
    if (s->notice && problem_code (s->notice, &problem))
      error = error_of (problem, BRO_SEVERITY_ERROR, NULL, error);
    return decided (BRO_OUTCOME_FAILED, error);
  }
  int read_errors = 0;
  for (guint i = 0; i < steps->len; i++)
    read_errors += ((BroStepResult *) steps->pdata[i])->read_errors;
  if (read_errors > 0) {
    BroJsonValue *p = bro_json_value_new_object ();
    bro_json_value_set (p, "count", bro_json_value_new_integer (read_errors));
    return decided (BRO_OUTCOME_SUCCEEDED_WITH_READ_ERRORS, error_of (BRO_MSG_RIP_READ_ERRORS, BRO_SEVERITY_ERROR, p, NULL));
  }
  for (guint i = 0; i < steps->len; i++) {
    const BroStepResult *s = steps->pdata[i];
    if (s->state == BRO_STEP_STATE_FAILED && s->affects_outcome)
      return decided (BRO_OUTCOME_SUCCEEDED_STEPS_FAILED, bro_bro_error_copy (s->error));
  }
  return decided (BRO_OUTCOME_SUCCEEDED, NULL);
}
