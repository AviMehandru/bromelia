/* bro-notes.c */
#include "bro-notes.h"
#include "bro-robot-message.h"

static void
add (GPtrArray *lines, BroMessageCode code, BroJsonValue *params, BroSeverity severity)
{
  g_ptr_array_add (lines, bro_bro_message_new (code, params, severity));
}

GPtrArray *
bro_notes_render (const BroId *job_id, BroOutcome outcome, const BroBroError *error, GPtrArray *read_errors, const char *const *logs)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  BroJsonValue *p = bro_json_value_new_object ();
  bro_json_value_set (p, "jobId", bro_json_value_new_string (job_id->value));
  bro_json_value_set (p, "outcome", bro_json_value_new_string (bro_outcome_to_wire (outcome)));
  add (lines, BRO_MSG_ARCHIVE_NOTE_TITLE, p, BRO_SEVERITY_INFO);
  add (lines, BRO_MSG_ARCHIVE_NOTE_NOT_FINISHED, NULL, BRO_SEVERITY_INFO);
  if (error) {
    BroMessageCode code;
    if (!bro_message_code_parse (error->code, &code))
      code = BRO_MSG_INTERNAL_UNEXPECTED;
    add (lines, code, bro_json_value_ref (error->params), BRO_SEVERITY_ERROR);
  }
  if (read_errors && read_errors->len) {
    add (lines, BRO_MSG_ARCHIVE_NOTE_READ_ERRORS, NULL, BRO_SEVERITY_INFO);
    for (guint i = 0; i < read_errors->len; i++) {
      BroJsonValue *t = bro_json_value_new_object ();
      bro_json_value_set (t, "text", bro_json_value_new_string (((BroRobotMessage *) read_errors->pdata[i])->text));
      add (lines, BRO_MSG_MAKEMKV_MESSAGE, t, BRO_SEVERITY_ERROR);
    }
  }
  if (logs && logs[0]) {
    BroJsonValue *l = bro_json_value_new_object ();
    bro_json_value_set (l, "log", bro_json_value_new_string (logs[0]));
    bro_json_value_set (l, "hasMakemkv", bro_json_value_new_bool (logs[1] != NULL));
    bro_json_value_set (l, "makemkvLog", bro_json_value_new_string (logs[1] ? logs[1] : ""));
    add (lines, BRO_MSG_ARCHIVE_NOTE_LOGS, l, BRO_SEVERITY_INFO);
  }
  return lines;
}
