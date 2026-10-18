/* bro-step-report.c */
#include "bro-step-report.h"

BroStepReport *
bro_step_report_new (BroJsonValue *data)
{
  BroStepReport *r = g_new0 (BroStepReport, 1);
  r->data = data ? data : bro_json_value_new_object ();
  r->append_steps = g_array_new (FALSE, FALSE, sizeof (BroStepKind));
  r->affects_outcome = TRUE;
  return r;
}

void
bro_step_report_free (BroStepReport *report)
{
  if (!report)
    return;
  bro_json_value_unref (report->data);
  bro_bro_message_free (report->summary);
  g_array_unref (report->append_steps);
  bro_makemkv_notice_free (report->notice);
  bro_bro_message_free (report->skip_job);
  bro_bro_error_free (report->error);
  bro_job_plan_free (report->plan);
  g_free (report);
}
