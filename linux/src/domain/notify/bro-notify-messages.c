/* bro-notify-messages.c */
#include "bro-notify-messages.h"

static BroBroMessage *
message (BroMessageCode code, BroJsonValue *params)
{
  return bro_bro_message_new (code, params, BRO_SEVERITY_INFO);
}

static BroJsonValue *
params (void)
{
  return bro_json_value_new_object ();
}

BroTitleAndBody *
bro_notify_messages_for_job (const BroJobSummary *job, BroOutcome outcome)
{
  BroJsonValue *t = params ();
  bro_json_value_set (t, "mode", bro_json_value_new_string (bro_rip_mode_to_wire (job->mode)));
  bro_json_value_set (t, "outcome", bro_json_value_new_string (bro_outcome_to_wire (outcome)));
  bro_json_value_set (t, "what", bro_json_value_new_string (job->what));
  BroTitleAndBody *r = bro_title_and_body_new (message (BRO_MSG_NOTIFY_TITLE_JOB, t));
  BroJsonValue *p = params ();
  BroMessageCode code;
  switch (outcome) {
  case BRO_OUTCOME_SUCCEEDED:
  case BRO_OUTCOME_SUCCEEDED_STEPS_FAILED:
    code = BRO_MSG_NOTIFY_BODY_SAVED;
    bro_json_value_set (p, "count", bro_json_value_new_integer (job->files));
    bro_json_value_set (p, "path", bro_json_value_new_string (job->path));
    break;
  case BRO_OUTCOME_SUCCEEDED_WITH_READ_ERRORS:
    code = BRO_MSG_NOTIFY_BODY_READ_ERRORS;
    bro_json_value_set (p, "path", bro_json_value_new_string (job->path));
    break;
  default: {
    code = BRO_MSG_NOTIFY_BODY_ERROR;
    g_autoptr (BroBroMessage) fallback = message (outcome == BRO_OUTCOME_CANCELLED     ? BRO_MSG_OUTCOME_CANCELLED
                                                  : outcome == BRO_OUTCOME_SKIPPED     ? BRO_MSG_OUTCOME_SKIPPED
                                                  : outcome == BRO_OUTCOME_INTERRUPTED ? BRO_MSG_OUTCOME_INTERRUPTED
                                                                                       : BRO_MSG_OUTCOME_FAILED,
                                                  NULL);
    bro_json_value_set (p, "error", bro_bro_message_to_json (job->error ? job->error : fallback));
    break;
  }
  }
  g_ptr_array_add (r->lines, message (code, p));
  return r;
}

BroTitleAndBody *
bro_notify_messages_for_check (const BroFolderCheck *results, gsize n_results)
{
  int bad = 0;
  for (gsize i = 0; i < n_results; i++)
    bad += results[i].result->result != BRO_CHECK_RESULT_OK;
  BroJsonValue *t = params ();
  if (bad == 0) {
    bro_json_value_set (t, "total", bro_json_value_new_integer ((gint64) n_results));
    BroTitleAndBody *r = bro_title_and_body_new (message (BRO_MSG_CHECK_NOTIFY_ALL_OK, t));
    g_ptr_array_add (r->lines, message (BRO_MSG_CHECK_NOTIFY_EVERY_FILE_MATCHES, NULL));
    return r;
  }
  bro_json_value_set (t, "damaged", bro_json_value_new_integer (bad));
  bro_json_value_set (t, "total", bro_json_value_new_integer ((gint64) n_results));
  BroTitleAndBody *r = bro_title_and_body_new (message (BRO_MSG_CHECK_NOTIFY_DAMAGED, t));
  int shown = 0;
  for (gsize i = 0; i < n_results && shown < 10; i++) {
    if (results[i].result->result == BRO_CHECK_RESULT_OK)
      continue;
    BroJsonValue *p = params ();
    bro_json_value_set (p, "folder", bro_json_value_new_string (results[i].folder));
    bro_json_value_set (p, "summary", bro_bro_message_to_json (results[i].result->summary));
    g_ptr_array_add (r->lines, message (BRO_MSG_CHECK_NOTIFY_FOLDER, p));
    shown++;
  }
  if (bad > 10) {
    BroJsonValue *p = params ();
    bro_json_value_set (p, "count", bro_json_value_new_integer (bad - 10));
    g_ptr_array_add (r->lines, message (BRO_MSG_CHECK_NOTIFY_MORE, p));
  }
  return r;
}
