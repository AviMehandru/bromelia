/* bro-name-output.c */
#include "bro-name-output.h"

static GPtrArray *
files_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) bro_named_file_free);
}

BroNameOutput *
bro_name_output_new (void)
{
  BroNameOutput *o = g_new0 (BroNameOutput, 1);
  o->folder = g_strdup ("");
  o->name = g_strdup ("");
  o->files = files_new ();
  return o;
}

void
bro_name_output_free (BroNameOutput *output)
{
  if (!output)
    return;
  g_free (output->folder);
  g_free (output->name);
  g_ptr_array_unref (output->files);
  bro_identity_free (output->identity);
  g_free (output);
}

BroJsonValue *
bro_name_output_encode (const BroNameOutput *output)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *files = bro_json_value_new_array ();
  for (guint i = 0; i < output->files->len; i++)
    bro_json_value_append (files, bro_named_file_encode (output->files->pdata[i]));
  bro_json_value_set (o, "folder", bro_json_value_new_string (output->folder));
  bro_json_value_set (o, "name", bro_json_value_new_string (output->name));
  bro_json_value_set (o, "merge", bro_json_value_new_bool (output->merge));
  bro_json_value_set (o, "files", files);
  if (output->identity)
    bro_json_value_set (o, "identity", bro_identity_encode (output->identity));
  return o;
}

BroNameOutput *
bro_name_output_decode (const BroJsonValue *json)
{
  BroNameOutput *o = bro_name_output_new ();
  BroJsonValue *files = bro_json_value_member (json, "files");
  BroJsonValue *identity = bro_json_value_member (json, "identity");
  g_free (o->folder);
  o->folder = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "folder"), ""));
  g_free (o->name);
  o->name = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "name"), ""));
  o->merge = bro_json_value_get_bool (bro_json_value_member (json, "merge"), FALSE);
  for (guint i = 0; i < bro_json_value_length (files); i++)
    g_ptr_array_add (o->files, bro_named_file_decode (bro_json_value_at (files, i)));
  if (identity && identity->kind == BRO_JSON_VALUE_OBJECT)
    o->identity = bro_identity_decode (identity);
  return o;
}

BroNameOutput *
bro_name_output_of (const BroJobSnapshot *job)
{
  BroJsonValue *json = bro_job_snapshot_output (job, BRO_STEP_KIND_NAME);
  return json && json->kind == BRO_JSON_VALUE_OBJECT ? bro_name_output_decode (json) : NULL;
}

GPtrArray *
bro_name_output_from_acquire (const BroAcquireOutput *acquired)
{
  GPtrArray *files = files_new ();
  for (guint i = 0; i < acquired->titles->len; i++)
    {
      const BroAcquiredTitle *t = acquired->titles->pdata[i];
      for (guint j = 0; j < t->files->len; j++)
        g_ptr_array_add (files, bro_named_file_new (t->files->pdata[j], "title", t->index));
    }
  for (guint i = 0; i < acquired->others->len; i++)
    g_ptr_array_add (files, bro_named_file_new (acquired->others->pdata[i], NULL, -1));
  if (acquired->backup)
    g_ptr_array_add (files, bro_named_file_new (acquired->backup, "backup", -1));
  if (acquired->disc_info)
    g_ptr_array_add (files, bro_named_file_new (acquired->disc_info, "discInfo", -1));
  return files;
}
