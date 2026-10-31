/* bro-system-status-dto.c */
#include "bro-system-status-dto.h"
#include "bro-library-dto.h"
#include "bro-problem-dto.h"
#include "bro-severity.h"

BroSystemStatusDto *
bro_system_status_dto_new (void)
{
  BroSystemStatusDto *s = g_new0 (BroSystemStatusDto, 1);
  s->tools = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_tool_info_free);
  s->problems = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_problem_dto_free);
  s->libraries = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_library_dto_free);
  return s;
}

void
bro_system_status_dto_free (BroSystemStatusDto *status)
{
  if (!status)
    return;
  bro_engine_facts_free (status->facts);
  g_free (status->makemkv_version);
  g_ptr_array_unref (status->tools);
  g_ptr_array_unref (status->problems);
  g_ptr_array_unref (status->libraries);
  g_free (status);
}

/* dtos.json ToolInfo: found when it has a path; why (with its severity) when the locator gave one. */
static BroJsonValue *
tool_json (const BroToolInfo *tool)
{
  BroJsonValue *m = bro_json_value_new_object ();
  bro_json_value_set (m, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (tool->tool)));
  bro_json_value_set (m, "found", bro_json_value_new_bool (tool->path != NULL));
  if (tool->path)
    bro_json_value_set (m, "path", bro_json_value_new_string (tool->path));
  if (tool->version)
    bro_json_value_set (m, "version", bro_json_value_new_string (tool->version));
  if (tool->why)
    {
      BroJsonValue *why = bro_bro_message_to_json (tool->why);
      bro_json_value_set (why, "severity", bro_json_value_new_string (bro_severity_to_wire (tool->why->severity)));
      bro_json_value_set (m, "why", why);
    }
  return m;
}

BroJsonValue *
bro_system_status_dto_json (const BroSystemStatusDto *status)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *engine = bro_json_value_new_object ();
  BroJsonValue *makemkv = bro_json_value_new_object ();
  BroJsonValue *tools = bro_json_value_new_array ();
  BroJsonValue *problems = bro_json_value_new_array ();
  BroJsonValue *libraries = bro_json_value_new_array ();
  g_autofree char *started = bro_instant_format (status->started_at);
  bro_json_value_set (o, "version", bro_json_value_new_string (status->facts->version));
  bro_json_value_set (engine, "pid", bro_json_value_new_integer (status->facts->pid));
  bro_json_value_set (engine, "startedAt", bro_json_value_new_string (started));
  bro_json_value_set (engine, "platform", bro_json_value_new_string (status->facts->platform));
  bro_json_value_set (engine, "dataDir", bro_json_value_new_string (status->facts->data_dir));
  bro_json_value_set (o, "engine", engine);
  bro_json_value_set (makemkv, "found", bro_json_value_new_bool (status->makemkv_found));
  if (status->makemkv_version)
    bro_json_value_set (makemkv, "version", bro_json_value_new_string (status->makemkv_version));
  bro_json_value_set (makemkv, "registration", bro_json_value_new_string ("unknown"));
  bro_json_value_set (o, "makemkv", makemkv);
  for (guint i = 0; i < status->tools->len; i++)
    bro_json_value_append (tools, tool_json (g_ptr_array_index (status->tools, i)));
  bro_json_value_set (o, "tools", tools);
  for (guint i = 0; i < status->problems->len; i++)
    bro_json_value_append (problems, bro_problem_dto_json (g_ptr_array_index (status->problems, i)));
  bro_json_value_set (o, "problems", problems);
  for (guint i = 0; i < status->libraries->len; i++)
    bro_json_value_append (libraries, bro_library_dto_json (g_ptr_array_index (status->libraries, i)));
  bro_json_value_set (o, "libraries", libraries);
  bro_json_value_set (o, "configVersion", bro_json_value_new_integer (status->config_version));
  bro_json_value_set (o, "eventSeq", bro_json_value_new_integer (status->event_seq));
  return o;
}
