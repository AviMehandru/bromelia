/* bro-run-accumulator.c */
#include "bro-run-accumulator.h"
#include "bro-message-catalog.h"
#include "bro-robot-private.h"

#include <string.h>

BroRunAccumulator *
bro_run_accumulator_new (gboolean reads_data, int expected_index, const char *expected_device)
{
  BroRunAccumulator *a = g_new0 (BroRunAccumulator, 1);
  a->errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  a->read_errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  a->reads_data = reads_data;
  a->expected_index = expected_index;
  a->expected_device = g_strdup (expected_device ? expected_device : "");
  return a;
}

void
bro_run_accumulator_free (BroRunAccumulator *a)
{
  if (!a)
    return;
  g_ptr_array_unref (a->errors);
  bro_robot_message_free (a->first_error);
  bro_robot_message_free (a->space_warning);
  g_ptr_array_unref (a->read_errors);
  bro_bro_message_free (a->drive_mismatch);
  g_free (a->debug_log);
  g_free (a->libre_drive);
  bro_makemkv_notice_free (a->problem);
  g_free (a->makemkv_version);
  bro_bro_message_free (a->stop_reason);
  g_free (a->expected_device);
  g_free (a);
}

static int
hex (char c)
{
  return g_ascii_xdigit_value (c);
}

/* The path of a file:// URL (percent-decoded; file:///C:/x → C:/x), else NULL. */
static char *
file_path (const char *url)
{
  if (g_ascii_strncasecmp (url, "file://", 7) != 0)
    return NULL;
  const char *rest = url + 7;
  if (g_ascii_strncasecmp (rest, "localhost/", 10) == 0)
    rest += 9;
  if (rest[0] != '/')
    return NULL;
  if (g_ascii_isalpha (rest[1]) && rest[2] == ':')
    rest++;
  GString *out = g_string_new (NULL);
  for (const char *p = rest; *p; p++) {
    if (*p == '%' && p[1] && p[2] && hex (p[1]) >= 0 && hex (p[2]) >= 0) {
      g_string_append_c (out, (char) (hex (p[1]) * 16 + hex (p[2])));
      p += 2;
    } else {
      g_string_append_c (out, *p);
    }
  }
  return g_string_free (out, FALSE);
}

static BroBroMessage *
message1 (BroMessageCode code, const char *key, BroJsonValue *value)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, key, value);
  return bro_bro_message_new (code, params, BRO_SEVERITY_ERROR);
}

static void
feed_message (BroRunAccumulator *a, const BroRobotMessage *m)
{
  g_autoptr (BroMakemkvNotice) notice = bro_message_catalog_notice (m);
  if (notice) {
    if (notice->kind == BRO_MAKEMKV_NOTICE_LIBRE_DRIVE) {
      if (!a->libre_drive)
        a->libre_drive = g_strdup (notice->detail);
    } else if (!a->problem) {
      a->problem = g_steal_pointer (&notice);
    }
  }
  /* "Debug logging enabled, log will be saved as file:///C:/…/MakeMKV_log.txt" */
  if (m->code == 1004 && m->params[0]) {
    char *path = file_path (m->params[0]);
    if (path) {
      g_free (a->debug_log);
      a->debug_log = path;
    }
  }
  if (bro_message_catalog_severity (m) == BRO_MESSAGE_KIND_ERROR) {
    if (!a->first_error && m->code != 5037 && m->code != 5004)
      a->first_error = bro_robot_message_copy (m);
    g_ptr_array_add (a->errors, bro_robot_message_copy (m));
    if (a->reads_data)
      g_ptr_array_add (a->read_errors, bro_robot_message_copy (m));
  }
  if (m->code == 1005 && !a->makemkv_version)
    a->makemkv_version = g_strdup (m->params[0] ? m->params[0] : m->text);
  int n;
  if ((m->code == 5036 || m->code == 5005) && m->params[0] && _bro_robot_parse_int (m->params[0], &n)) {
    a->has_saved = TRUE;
    a->saved = n;
  }
  if (m->code == 5038 && !a->space_warning) {
    /* "The total size of all output files may reach as much as … while there are only … free": stop before
     * anything is written rather than fail when the disk fills up. */
    a->space_warning = bro_robot_message_copy (m);
    if (!a->stop_reason)
      a->stop_reason = message1 (BRO_MSG_SPACE_MAKEMKV_WARNING, "text", bro_json_value_new_string (m->text));
  }
  if ((m->code == 5037 || m->code == 5004) && m->params[0] && m->params[1]) {
    if (_bro_robot_parse_int (m->params[0], &n)) {
      a->has_saved = TRUE;
      a->saved = n;
    }
    if (_bro_robot_parse_int (m->params[1], &n)) {
      a->has_failed = TRUE;
      a->failed = n;
    }
  }
}

void
bro_run_accumulator_feed (BroRunAccumulator *a, const BroRobotEvent *event)
{
  if (event->kind == BRO_ROBOT_EVENT_MESSAGE) {
    feed_message (a, event->message);
    return;
  }
  if (event->kind == BRO_ROBOT_EVENT_DRIVE && a->expected_index >= 0 && event->index == a->expected_index
      && a->expected_device[0] && event->device[0] && g_ascii_strcasecmp (event->device, a->expected_device) != 0) {
    if (!a->drive_mismatch) {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "index", bro_json_value_new_integer (a->expected_index));
      bro_json_value_set (params, "now", bro_json_value_new_string (event->device));
      bro_json_value_set (params, "expected", bro_json_value_new_string (a->expected_device));
      a->drive_mismatch = bro_bro_message_new (BRO_MSG_DRIVE_RENUMBERED, params, BRO_SEVERITY_ERROR);
    }
    if (!a->stop_reason)
      a->stop_reason = bro_bro_message_copy (a->drive_mismatch);
  }
}
