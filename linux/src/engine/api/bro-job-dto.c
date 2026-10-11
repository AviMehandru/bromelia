/* bro-job-dto.c */
#include "bro-job-dto.h"

BroJobDto *
bro_job_dto_new (void)
{
  return g_new0 (BroJobDto, 1);
}

void
bro_job_dto_free (BroJobDto *job)
{
  if (!job)
    return;
  g_free (job->drive_id);
  g_free (job->session_id);
  g_free (job->title);
  g_free (job->mode);
  bro_progress_dto_free (job->progress);
  bro_bro_message_free (job->blocked_by);
  bro_decision_dto_free (job->decision);
  bro_bro_error_free (job->error);
  g_free (job->output_path);
  g_free (job->fingerprint);
  g_free (job->counts);
  g_free (job);
}

/* {code, params, cause} (shared/schema/common.json BroError), params left out when empty. */
static BroJsonValue *
error_json (const BroBroError *error)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "code", bro_json_value_new_string (error->code));
  if (bro_json_value_length (error->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (error->params));
  if (error->cause)
    bro_json_value_set (o, "cause", error_json (error->cause));
  return o;
}

static void
set_instant (BroJsonValue *o, const char *key, BroInstant at)
{
  g_autofree char *text = bro_instant_format (at);
  bro_json_value_set (o, key, bro_json_value_new_string (text));
}

BroJsonValue *
bro_job_dto_json (const BroJobDto *job)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "id", bro_json_value_new_string (job->id.value));
  bro_json_value_set (o, "kind", bro_json_value_new_string (bro_job_kind_to_wire (job->kind)));
  bro_json_value_set (o, "state", bro_json_value_new_string (bro_job_state_to_wire (job->state)));
  if (job->has_outcome)
    bro_json_value_set (o, "outcome", bro_json_value_new_string (bro_outcome_to_wire (job->outcome)));
  bro_json_value_set (o, "queue", bro_json_value_new_string (bro_queue_to_wire (job->queue)));
  if (job->has_position)
    bro_json_value_set (o, "position", bro_json_value_new_integer (job->position));
  if (job->has_parent_id)
    bro_json_value_set (o, "parentId", bro_json_value_new_string (job->parent_id.value));
  if (job->drive_id)
    bro_json_value_set (o, "driveId", bro_json_value_new_string (job->drive_id));
  if (job->session_id)
    bro_json_value_set (o, "sessionId", bro_json_value_new_string (job->session_id->value));
  bro_json_value_set (o, "title", bro_json_value_new_string (job->title ? job->title : ""));
  if (job->mode)
    bro_json_value_set (o, "mode", bro_json_value_new_string (job->mode));
  bro_json_value_set (o, "automatic", bro_json_value_new_bool (job->automatic));
  set_instant (o, "createdAt", job->created_at);
  if (job->has_started_at)
    set_instant (o, "startedAt", job->started_at);
  if (job->has_finished_at)
    set_instant (o, "finishedAt", job->finished_at);
  if (job->progress)
    bro_json_value_set (o, "progress", bro_progress_dto_json (job->progress));
  if (job->blocked_by)
    bro_json_value_set (o, "blockedBy", bro_bro_message_to_json (job->blocked_by));
  if (job->decision)
    bro_json_value_set (o, "decision", bro_decision_dto_json (job->decision));
  if (job->error)
    bro_json_value_set (o, "error", error_json (job->error));
  if (job->has_unit_id)
    bro_json_value_set (o, "unitId", bro_json_value_new_string (job->unit_id.value));
  if (job->output_path)
    bro_json_value_set (o, "outputPath", bro_json_value_new_string (job->output_path));
  if (job->fingerprint)
    bro_json_value_set (o, "fingerprint", bro_json_value_new_string (job->fingerprint));
  if (job->counts)
    {
      BroJsonValue *c = bro_json_value_new_object ();
      bro_json_value_set (c, "files", bro_json_value_new_integer (job->counts->files));
      bro_json_value_set (c, "warnings", bro_json_value_new_integer (job->counts->warnings));
      bro_json_value_set (c, "errors", bro_json_value_new_integer (job->counts->errors));
      bro_json_value_set (c, "readErrors", bro_json_value_new_integer (job->counts->read_errors));
      bro_json_value_set (o, "counts", c);
    }
  return o;
}
