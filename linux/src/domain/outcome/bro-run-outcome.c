/* bro-run-outcome.c */
#include "bro-run-outcome.h"

#include <string.h>

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
  if (o->produced)
    g_ptr_array_unref (o->produced);
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

static gboolean
has_suffix_ignoring_case (const char *name, const char *suffix)
{
  gsize n = strlen (name), k = strlen (suffix);
  return n >= k && g_ascii_strcasecmp (name + n - k, suffix) == 0;
}

GPtrArray *
bro_run_outcome_products (BroRunProduct product, GPtrArray *new_names)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; new_names && i < new_names->len; i++)
    {
      const char *name = new_names->pdata[i];
      gboolean counts = FALSE;
      if (name[0] == '.')
        continue;
      if (product == BRO_RUN_PRODUCT_TITLES)
        counts = has_suffix_ignoring_case (name, ".mkv");
      else if (product == BRO_RUN_PRODUCT_BACKUP)
        counts = g_ascii_strcasecmp (name, "BDMV") == 0 || g_ascii_strcasecmp (name, "VIDEO_TS") == 0 || g_ascii_strcasecmp (name, "HVDVD_TS") == 0
                 || has_suffix_ignoring_case (name, ".iso");
      if (counts)
        g_ptr_array_add (out, g_strdup (name));
    }
  return out;
}

BroRunOutcome *
bro_run_outcome_classify (const BroRunAccumulator *a, const BroProcessExit *exit, BroRunProduct product, GPtrArray *new_names)
{
  BroRunOutcome *o = g_new0 (BroRunOutcome, 1);
  o->produced = bro_run_outcome_products (product, new_names);
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
  } else if (exit->status != 0 || (a->has_failed && a->failed > 0) || (product != BRO_RUN_PRODUCT_NOTHING && o->produced->len == 0)) {
    o->status = BRO_STATUS_WORD_FAILED;
    const BroRobotMessage *reason = a->first_error ? a->first_error
                                    : a->errors->len ? a->errors->pdata[a->errors->len - 1] : NULL;
    o->error = reason              ? message (BRO_MSG_MAKEMKV_MESSAGE, "text", bro_json_value_new_string (reason->text), NULL, NULL)
               : exit->status != 0 ? message (BRO_MSG_PROCESS_EXIT_STATUS, "status", bro_json_value_new_integer (exit->status), NULL, NULL)
                                   : message (BRO_MSG_PROCESS_SAVED_NOTHING, "tool", bro_json_value_new_string ("makemkvcon"), NULL, NULL);
  } else if (product == BRO_RUN_PRODUCT_TITLES && !a->has_saved) {
    o->status = BRO_STATUS_WORD_FAILED;
    o->error = message (BRO_MSG_MAKEMKV_NO_SUMMARY, NULL, NULL, NULL, NULL);
  } else if (product == BRO_RUN_PRODUCT_TITLES && a->saved != (int) o->produced->len) {
    o->status = BRO_STATUS_WORD_FAILED;
    o->error = message (BRO_MSG_MAKEMKV_SAVED_MISMATCH, "saved", bro_json_value_new_integer (a->saved), "produced",
                        bro_json_value_new_integer (o->produced->len));
  } else if (a->read_errors->len > 0) {
    o->status = BRO_STATUS_WORD_ERRORS;
    o->error = message (BRO_MSG_RIP_READ_ERRORS, "count", bro_json_value_new_integer (a->read_errors->len), NULL, NULL);
  } else {
    o->status = BRO_STATUS_WORD_SUCCESS;
  }
  return o;
}
