/* bro-job-request.c */
#include "bro-job-request.h"

BroJobRequest *
bro_job_request_new (BroJobKind kind, const char *title)
{
  BroJobRequest *r = g_new0 (BroJobRequest, 1);
  r->kind = kind;
  r->title = g_strdup (title ? title : "");
  return r;
}

void
bro_job_request_free (BroJobRequest *request)
{
  if (!request)
    return;
  g_free (request->title);
  g_free (request->drive_id);
  g_free (request->session_id);
  g_free (request->parent_id);
  g_free (request->unit_id);
  bro_json_value_unref (request->options);
  bro_json_value_unref (request->details);
  g_free (request);
}

static BroId *
copy_id (const BroId *id)
{
  return id ? g_memdup2 (id, sizeof *id) : NULL;
}

BroJobRequest *
bro_job_request_copy (const BroJobRequest *request)
{
  BroJobRequest *r;
  if (!request)
    return NULL;
  r = bro_job_request_new (request->kind, request->title);
  r->automatic = request->automatic;
  r->drive_id = g_strdup (request->drive_id);
  r->has_media_generation = request->has_media_generation;
  r->media_generation = request->media_generation;
  r->session_id = copy_id (request->session_id);
  r->parent_id = copy_id (request->parent_id);
  r->unit_id = copy_id (request->unit_id);
  r->options = request->options ? bro_json_value_ref (request->options) : NULL;
  r->details = request->details ? bro_json_value_ref (request->details) : NULL;
  return r;
}
