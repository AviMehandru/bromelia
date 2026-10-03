/* bro-step-definition.c */
#include "bro-step-definition.h"
#include "bro-config-private.h"
#include "bro-issue.h"

#define M bro_json_value_member
#define STR(o, k) g_strdup (bro_json_value_get_string (M ((o), (k)), ""))

BroStepDefinition *
bro_step_definition_decode (BroJsonValue *json)
{
  g_autoptr (GPtrArray) issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  BroJsonValue *node = _bro_schema_def ("Step");
  g_autoptr (BroJsonValue) j = _bro_schema_normalize (json, node, "", TRUE, issues);
  BroStepDefinition *s = g_new0 (BroStepDefinition, 1);
  s->id = STR (j, "id");
  s->kind = BRO_STEP_TYPE_COMMAND;
  bro_step_type_from_wire (bro_json_value_get_string (M (j, "kind"), ""), &s->kind);
  s->name = STR (j, "name");
  s->enabled = bro_json_value_get_bool (M (j, "enabled"), TRUE);
  s->run_on = BRO_RUN_ON_SUCCESS;
  bro_run_on_from_wire (bro_json_value_get_string (M (j, "runOn"), ""), &s->run_on);
  s->background = bro_json_value_get_bool (M (j, "background"), s->kind == BRO_STEP_TYPE_HANDBRAKE);
  s->affects_outcome = bro_json_value_get_bool (M (j, "affectsOutcome"), FALSE);
  s->timeout_seconds = bro_json_value_get_integer (M (j, "timeoutSeconds"), 0);
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  BroJsonValue *props = M (node, "properties");
  g_autoptr (BroJsonValue) c = _bro_schema_normalize (M (j, "command") ? M (j, "command") : empty, M (props, "command"), "", TRUE, issues);
  s->command.executable = STR (c, "executable");
  s->command.interpreter = STR (c, "interpreter");
  s->command.arguments = STR (c, "arguments");
  s->command.working_directory = STR (c, "workingDirectory");
  s->command.per_file = bro_json_value_get_bool (M (c, "perFile"), FALSE);
  s->command.environment = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  BroJsonValue *env = M (c, "environment");
  for (guint i = 0; env && env->keys && i < env->keys->len; i++)
    g_hash_table_insert (s->command.environment, g_strdup (env->keys->pdata[i]), g_strdup (bro_json_value_get_string (env->items->pdata[i], "")));
  g_autoptr (BroJsonValue) h = _bro_schema_normalize (M (j, "handbrake") ? M (j, "handbrake") : empty, M (props, "handbrake"), "", TRUE, issues);
  s->handbrake.executable = STR (h, "executable");
  s->handbrake.preset = STR (h, "preset");
  s->handbrake.preset_file = STR (h, "presetFile");
  s->handbrake.output_path = STR (h, "outputPath");
  s->handbrake.extra_arguments = STR (h, "extraArguments");
  return s;
}

void
bro_step_definition_free (BroStepDefinition *s)
{
  if (!s)
    return;
  g_free (s->id);
  g_free (s->name);
  g_free (s->command.executable);
  g_free (s->command.interpreter);
  g_free (s->command.arguments);
  g_free (s->command.working_directory);
  g_hash_table_unref (s->command.environment);
  g_free (s->handbrake.executable);
  g_free (s->handbrake.preset);
  g_free (s->handbrake.preset_file);
  g_free (s->handbrake.output_path);
  g_free (s->handbrake.extra_arguments);
  g_free (s);
}
