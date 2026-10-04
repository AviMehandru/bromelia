/* bro-job-record.c */
#include "bro-job-record.h"

BroJobRecord *
bro_job_record_new (void)
{
  BroJobRecord *x = g_new0 (BroJobRecord, 1);
  return x;
}

void
bro_job_record_free (BroJobRecord *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->error, bro_bro_error_free);
  g_clear_pointer (&x->blocked_by, bro_json_value_unref);
  g_clear_pointer (&x->decision, bro_json_value_unref);
  g_free (x->drive_id);
  g_free (x->mode);
  g_free (x->title);
  g_free (x->fingerprint);
  g_clear_pointer (&x->request, bro_job_request_free);
  g_clear_pointer (&x->plan, bro_job_plan_free);
  g_free (x);
}
