/* test-engine-log.inc: shared/fixtures/adapters/engine-log.cases.json (FileEngineLog). Included by test-adapters.c,
 * which provides fs_root. */
#include "bro-file-engine-log.h"
#include <sys/stat.h>

static gboolean
engine_log_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (TestManualClock) clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  g_autoptr (BroFileEngineLog) log = NULL;
  g_autofree char *data = g_build_filename (fs_root, "data", NULL);
  g_autofree char *logs = g_build_filename (data, "logs", NULL);
  g_autofree char *path = g_build_filename (logs, "engine.log", NULL);
  BroJsonValue *lines = bro_json_value_member (given, "lines");
  const char *before = bro_json_value_get_string (bro_json_value_member (given, "before"), NULL);
  const char *want = bro_json_value_get_string (bro_json_value_member (expect, "file"), NULL);
  BroJsonValue *mode = bro_json_value_member (expect, "mode");
  bro_test_remove_tree (fs_root);
  g_mkdir_with_parents (bro_json_value_get_bool (bro_json_value_member (given, "noLogsFolder"), FALSE) ? data : logs, 0755);
  if (before)
    g_file_set_contents (path, before, -1, NULL);
  if (bro_json_value_get_bool (bro_json_value_member (given, "logIsAFolder"), FALSE))
    g_mkdir (path, 0755);
  log = bro_file_engine_log_new (BRO_FILE_SYSTEM (fs), BRO_CLOCK (clock), path);
  for (guint i = 0; i < bro_json_value_length (lines); i++)
    {
      BroJsonValue *line = bro_json_value_at (lines, i);
      BroJsonValue *params = bro_json_value_member (line, "params");
      BroMessageCode code;
      BroSeverity severity;
      g_autoptr (BroBroMessage) message = NULL;
      if (!bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (line, "code"), ""), &code)
          || !bro_severity_from_wire (bro_json_value_get_string (bro_json_value_member (line, "severity"), ""), &severity))
        {
          bro_test_fail (failures, id, "line %u: unknown code or severity", i);
          return TRUE;
        }
      test_manual_clock_advance (clock, bro_json_value_get_integer (bro_json_value_member (line, "at"), 0) - clock->now_ms, NULL, NULL);
      message = bro_bro_message_new (code, params ? bro_json_value_ref (params) : NULL, severity);
      bro_file_engine_log_log (log, message);
    }
  if (bro_json_value_get_bool (bro_json_value_member (expect, "logIsAFolder"), FALSE) && !g_file_test (path, G_FILE_TEST_IS_DIR))
    bro_test_fail (failures, id, "engine.log is no longer a folder");
  if (want)
    {
      g_autofree char *text = NULL;
      if (!g_file_get_contents (path, &text, NULL, NULL))
        bro_test_fail (failures, id, "engine.log can't be read");
      else if (strcmp (text, want) != 0)
        bro_test_fail (failures, id, "engine.log is\n%s\nnot\n%s", text, want);
    }
  if (mode)
    {
      struct stat st;
      if (stat (path, &st) != 0 || (gint64) (st.st_mode & 0777) != bro_json_value_get_integer (mode, -1))
        bro_test_fail (failures, id, "engine.log's mode is %o", (unsigned) (st.st_mode & 0777));
    }
  return TRUE;
}

static void
test_engine_log (void)
{
  bro_test_run_cases ("adapters/engine-log.cases.json", engine_log_case);
}
