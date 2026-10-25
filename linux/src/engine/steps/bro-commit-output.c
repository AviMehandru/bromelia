/* bro-commit-output.c */
#include "bro-commit-output.h"

BroCommitOutput *
bro_commit_output_new (void)
{
  BroCommitOutput *o = g_new0 (BroCommitOutput, 1);
  o->library = g_strdup ("");
  o->path = g_strdup ("");
  return o;
}

void
bro_commit_output_free (BroCommitOutput *output)
{
  if (!output)
    return;
  g_free (output->library);
  g_free (output->path);
  g_free (output);
}

BroJsonValue *
bro_commit_output_encode (const BroCommitOutput *output)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "unitId", bro_json_value_new_string (output->unit_id.value));
  bro_json_value_set (o, "library", bro_json_value_new_string (output->library));
  bro_json_value_set (o, "path", bro_json_value_new_string (output->path));
  bro_json_value_set (o, "merged", bro_json_value_new_bool (output->merged));
  bro_json_value_set (o, "quarantined", bro_json_value_new_bool (output->quarantined));
  return o;
}

BroCommitOutput *
bro_commit_output_decode (const BroJsonValue *json)
{
  BroCommitOutput *o = bro_commit_output_new ();
  g_strlcpy (o->unit_id.value, bro_json_value_get_string (bro_json_value_member (json, "unitId"), ""), sizeof o->unit_id.value);
  g_free (o->library);
  o->library = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "library"), ""));
  g_free (o->path);
  o->path = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "path"), ""));
  o->merged = bro_json_value_get_bool (bro_json_value_member (json, "merged"), FALSE);
  o->quarantined = bro_json_value_get_bool (bro_json_value_member (json, "quarantined"), FALSE);
  return o;
}

BroCommitOutput *
bro_commit_output_of (const BroJobSnapshot *job)
{
  BroJsonValue *json = bro_job_snapshot_output (job, BRO_STEP_KIND_COMMIT);
  return json && json->kind == BRO_JSON_VALUE_OBJECT ? bro_commit_output_decode (json) : NULL;
}
