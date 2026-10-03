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
