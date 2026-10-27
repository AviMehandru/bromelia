/* bro-job-detail-dto.c */
#include "bro-job-detail-dto.h"

BroJobDetailDto *
bro_job_detail_dto_new (BroJobDto *job)
{
  BroJobDetailDto *d = g_new0 (BroJobDetailDto, 1);
  d->job = job;
  d->steps = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_job_step_dto_free);
  d->files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_job_file_dto_free);
  d->commands = g_ptr_array_new_with_free_func (g_free);
  return d;
}

void
bro_job_detail_dto_free (BroJobDetailDto *detail)
{
  if (!detail)
    return;
  bro_job_dto_free (detail->job);
  g_ptr_array_unref (detail->steps);
  g_ptr_array_unref (detail->files);
  g_ptr_array_unref (detail->commands);
  g_free (detail);
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

static BroJsonValue *
step_json (const BroJobStepDto *step)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "seq", bro_json_value_new_integer (step->seq));
  bro_json_value_set (o, "kind", bro_json_value_new_string (bro_step_kind_to_wire (step->kind)));
  bro_json_value_set (o, "state", bro_json_value_new_string (bro_step_state_to_wire (step->state)));
  if (step->has_started_at)
    set_instant (o, "startedAt", step->started_at);
  if (step->has_finished_at)
    set_instant (o, "finishedAt", step->finished_at);
  if (step->error)
    bro_json_value_set (o, "error", error_json (step->error));
  if (step->summary)
    bro_json_value_set (o, "summary", bro_bro_message_to_json (step->summary));
  return o;
}

static BroJsonValue *
file_json (const BroJobFileDto *file)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "path", bro_json_value_new_string (file->path));
  if (file->has_size)
    bro_json_value_set (o, "size", bro_json_value_new_integer (file->size));
  if (file->sha256)
    bro_json_value_set (o, "sha256", bro_json_value_new_string (file->sha256));
  if (file->role)
    bro_json_value_set (o, "role", bro_json_value_new_string (file->role));
  return o;
}

BroJsonValue *
bro_job_detail_dto_json (const BroJobDetailDto *detail)
{
  BroJsonValue *o = bro_job_dto_json (detail->job);
  BroJsonValue *steps = bro_json_value_new_array ();
  BroJsonValue *files = bro_json_value_new_array ();
  BroJsonValue *commands = bro_json_value_new_array ();
  for (guint i = 0; i < detail->steps->len; i++)
    bro_json_value_append (steps, step_json (g_ptr_array_index (detail->steps, i)));
  for (guint i = 0; i < detail->files->len; i++)
    bro_json_value_append (files, file_json (g_ptr_array_index (detail->files, i)));
  for (guint i = 0; i < detail->commands->len; i++)
    bro_json_value_append (commands, bro_json_value_new_string (g_ptr_array_index (detail->commands, i)));
  bro_json_value_set (o, "steps", steps);
  bro_json_value_set (o, "files", files);
  bro_json_value_set (o, "commands", commands);
  bro_json_value_set (o, "logBytes", bro_json_value_new_integer (detail->log_bytes));
  return o;
}
