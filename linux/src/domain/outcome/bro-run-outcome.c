/* bro-run-outcome.c */
#include "bro-run-outcome.h"

void
bro_run_outcome_free (BroRunOutcome *o)
{
  if (!o)
    return;
  bro_robot_message_free (o->first_error);
  bro_robot_message_free (o->space_warning);
  g_ptr_array_unref (o->read_errors);
  bro_bro_message_free (o->drive_mismatch);
  g_free (o->debug_log);
  bro_bro_message_free (o->error);
  g_free (o);
}

static BroBroMessage *
message (BroMessageCode code, const char *key, BroJsonValue *value, const char *key2, BroJsonValue *value2)
{
  BroJsonValue *p = bro_json_value_new_object ();
  if (key)
    bro_json_value_set (p, key, value);
  if (key2)
    bro_json_value_set (p, key2, value2);
  return bro_bro_message_new (code, p, BRO_SEVERITY_ERROR);
}

BroRunOutcome *
bro_run_outcome_classify (const BroRunAccumulator *a, const BroProcessExit *exit, int produced_files)
{
  BroRunOutcome *o = g_new0 (BroRunOutcome, 1);
  o->has_saved = a->has_saved;
  o->saved = a->saved;
  o->has_failed = a->has_failed;
  o->failed = a->failed;
  o->exit_code = exit->status;
  o->first_error = bro_robot_message_copy (a->first_error);
  o->space_warning = bro_robot_message_copy (a->space_warning);
  o->read_errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  for (guint i = 0; i < a->read_errors->len; i++)
    g_ptr_array_add (o->read_errors, bro_robot_message_copy (a->read_errors->pdata[i]));
  o->drive_mismatch = bro_bro_message_copy (a->drive_mismatch);
  o->debug_log = g_strdup (a->debug_log);

  /* These stop makemkvcon themselves, so they come before cancellation. */
  if (a->drive_mismatch) {
    o->status = BRO_STATUS_WORD_FAILED;
    o->error = bro_bro_message_copy (a->drive_mismatch);
  } else if (a->space_warning) {
    o->status = BRO_STATUS_WORD_FAILED;
    o->error = message (BRO_MSG_SPACE_MAKEMKV_WARNING, "text", bro_json_value_new_string (a->space_warning->text), NULL, NULL);
  } else if (exit->stalled_seconds >= 0) {
    o->status = BRO_STATUS_WORD_FAILED;
    o->error = message (BRO_MSG_MAKEMKV_STALLED, "minutes", bro_json_value_new_integer ((gint64) (exit->stalled_seconds / 60)),
                        "abandoned", bro_json_value_new_bool (exit->abandoned));
  } else if (exit->cancelled || exit->abandoned) {
    o->status = BRO_STATUS_WORD_CANCELLED;
  } else if (exit->status != 0 || (a->has_failed && a->failed > 0) || produced_files == 0) {
    o->status = BRO_STATUS_WORD_FAILED;
    const BroRobotMessage *reason = a->first_error ? a->first_error
                                    : a->errors->len ? a->errors->pdata[a->errors->len - 1] : NULL;
    o->error = reason ? message (BRO_MSG_MAKEMKV_MESSAGE, "text", bro_json_value_new_string (reason->text), NULL, NULL)
                      : message (BRO_MSG_PROCESS_EXIT_STATUS, "status", bro_json_value_new_integer (exit->status), NULL, NULL);
  } else if (a->read_errors->len > 0) {
    o->status = BRO_STATUS_WORD_ERRORS;
    o->error = message (BRO_MSG_RIP_READ_ERRORS, "count", bro_json_value_new_integer (a->read_errors->len), NULL, NULL);
  } else {
    o->status = BRO_STATUS_WORD_SUCCESS;
  }
  return o;
}
