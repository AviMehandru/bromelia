/* bro-seal-output.c */
#include "bro-seal-output.h"

BroSealOutput *
bro_seal_output_new (void)
{
  BroSealOutput *o = g_new0 (BroSealOutput, 1);
  o->status = g_strdup ("");
  o->files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  o->record = g_strdup ("");
  o->logs = g_ptr_array_new_with_free_func (g_free);
  return o;
}

void
bro_seal_output_free (BroSealOutput *output)
{
  if (!output)
    return;
  g_free (output->status);
  g_ptr_array_unref (output->files);
  g_free (output->record);
  g_ptr_array_unref (output->logs);
  g_free (output->note);
  g_free (output->tag);
  g_free (output);
}

static BroJsonValue *
string_or_null (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

BroJsonValue *
_bro_seal_output_file (const BroUnitFile *file)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "path", bro_json_value_new_string (file->path));
  bro_json_value_set (o, "size", bro_json_value_new_integer (file->size));
  bro_json_value_set (o, "sha256", bro_json_value_new_string (file->sha256));
  if (file->role && *file->role)
    bro_json_value_set (o, "role", bro_json_value_new_string (file->role));
  if (file->has_title)
    bro_json_value_set (o, "title", bro_json_value_new_integer (file->title));
  return o;
}

BroJsonValue *
bro_seal_output_encode (const BroSealOutput *output)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *files = bro_json_value_new_array ();
  BroJsonValue *logs = bro_json_value_new_array ();
  for (guint i = 0; i < output->files->len; i++)
    bro_json_value_append (files, _bro_seal_output_file (output->files->pdata[i]));
  for (guint i = 0; i < output->logs->len; i++)
    bro_json_value_append (logs, bro_json_value_new_string (output->logs->pdata[i]));
  bro_json_value_set (o, "unitId", bro_json_value_new_string (output->unit_id.value));
  bro_json_value_set (o, "status", bro_json_value_new_string (output->status));
  bro_json_value_set (o, "files", files);
  bro_json_value_set (o, "record", bro_json_value_new_string (output->record));
  bro_json_value_set (o, "logs", logs);
  bro_json_value_set (o, "note", string_or_null (output->note));
  bro_json_value_set (o, "tag", string_or_null (output->tag));
  return o;
}

BroSealOutput *
bro_seal_output_decode (const BroJsonValue *json)
{
  BroSealOutput *o = bro_seal_output_new ();
  BroJsonValue *files = bro_json_value_member (json, "files");
  BroJsonValue *logs = bro_json_value_member (json, "logs");
  g_strlcpy (o->unit_id.value, bro_json_value_get_string (bro_json_value_member (json, "unitId"), ""), sizeof o->unit_id.value);
  g_free (o->status);
  o->status = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "status"), ""));
  for (guint i = 0; i < bro_json_value_length (files); i++)
    {
      BroJsonValue *f = bro_json_value_at (files, i);
      BroJsonValue *title = bro_json_value_member (f, "title");
      BroUnitFile *u = bro_unit_file_new ();
      u->path = g_strdup (bro_json_value_get_string (bro_json_value_member (f, "path"), ""));
      u->size = bro_json_value_get_integer (bro_json_value_member (f, "size"), 0);
      u->sha256 = g_strdup (bro_json_value_get_string (bro_json_value_member (f, "sha256"), ""));
      u->role = g_strdup (bro_json_value_get_string (bro_json_value_member (f, "role"), ""));
      u->has_title = title && title->kind == BRO_JSON_VALUE_INTEGER;
      u->title = u->has_title ? (int) title->integer : 0;
      g_ptr_array_add (o->files, u);
    }
  g_free (o->record);
  o->record = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "record"), ""));
  for (guint i = 0; i < bro_json_value_length (logs); i++)
    g_ptr_array_add (o->logs, g_strdup (bro_json_value_get_string (bro_json_value_at (logs, i), "")));
  o->note = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "note"), NULL));
  o->tag = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "tag"), NULL));
  return o;
}

BroSealOutput *
bro_seal_output_of (const BroJobSnapshot *job)
{
  BroJsonValue *json = bro_job_snapshot_output (job, BRO_STEP_KIND_SEAL);
  return json && json->kind == BRO_JSON_VALUE_OBJECT ? bro_seal_output_decode (json) : NULL;
}
