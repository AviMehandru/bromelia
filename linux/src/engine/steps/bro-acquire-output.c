/* bro-acquire-output.c */
#include "bro-acquire-output.h"

BroAcquireOutput *
bro_acquire_output_new (void)
{
  BroAcquireOutput *o = g_new0 (BroAcquireOutput, 1);
  o->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_acquired_title_free);
  o->others = g_ptr_array_new_with_free_func (g_free);
  o->read_errors = g_ptr_array_new_with_free_func (g_free);
  return o;
}

void
bro_acquire_output_free (BroAcquireOutput *output)
{
  if (!output)
    return;
  g_ptr_array_unref (output->titles);
  g_ptr_array_unref (output->others);
  g_free (output->backup);
  g_free (output->disc_info);
  g_ptr_array_unref (output->read_errors);
  g_free (output);
}

static BroJsonValue *
strings (GPtrArray *list)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < list->len; i++)
    bro_json_value_append (a, bro_json_value_new_string (list->pdata[i]));
  return a;
}

static BroJsonValue *
string_or_null (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

static void
strings_of (const BroJsonValue *json, GPtrArray *out)
{
  for (guint i = 0; i < bro_json_value_length (json); i++)
    g_ptr_array_add (out, g_strdup (bro_json_value_get_string (bro_json_value_at (json, i), "")));
}

BroJsonValue *
bro_acquire_output_encode (const BroAcquireOutput *output)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *titles = bro_json_value_new_array ();
  for (guint i = 0; i < output->titles->len; i++)
    bro_json_value_append (titles, bro_acquired_title_encode (output->titles->pdata[i]));
  bro_json_value_set (o, "titles", titles);
  bro_json_value_set (o, "others", strings (output->others));
  bro_json_value_set (o, "backup", string_or_null (output->backup));
  bro_json_value_set (o, "discInfo", string_or_null (output->disc_info));
  bro_json_value_set (o, "readErrors", strings (output->read_errors));
  return o;
}

BroAcquireOutput *
bro_acquire_output_decode (const BroJsonValue *json)
{
  BroAcquireOutput *o = bro_acquire_output_new ();
  BroJsonValue *titles = bro_json_value_member (json, "titles");
  for (guint i = 0; i < bro_json_value_length (titles); i++)
    g_ptr_array_add (o->titles, bro_acquired_title_decode (bro_json_value_at (titles, i)));
  strings_of (bro_json_value_member (json, "others"), o->others);
  o->backup = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "backup"), NULL));
  o->disc_info = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "discInfo"), NULL));
  strings_of (bro_json_value_member (json, "readErrors"), o->read_errors);
  return o;
}

BroAcquireOutput *
bro_acquire_output_of (const BroJobSnapshot *job)
{
  BroJsonValue *json = bro_job_snapshot_output (job, BRO_STEP_KIND_ACQUIRE);
  return json && json->kind == BRO_JSON_VALUE_OBJECT ? bro_acquire_output_decode (json) : NULL;
}

GPtrArray *
bro_acquire_output_produced (const BroAcquireOutput *output)
{
  GPtrArray *all = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < output->titles->len; i++)
    {
      BroAcquiredTitle *t = output->titles->pdata[i];
      for (guint j = 0; j < t->files->len; j++)
        g_ptr_array_add (all, g_strdup (t->files->pdata[j]));
    }
  for (guint i = 0; i < output->others->len; i++)
    g_ptr_array_add (all, g_strdup (output->others->pdata[i]));
  if (output->backup)
    g_ptr_array_add (all, g_strdup (output->backup));
  if (output->disc_info)
    g_ptr_array_add (all, g_strdup (output->disc_info));
  return all;
}

int
bro_acquire_output_read_error_count (const BroAcquireOutput *output)
{
  int count = output->read_errors->len;
  for (guint i = 0; i < output->titles->len; i++)
    count += ((BroAcquiredTitle *) output->titles->pdata[i])->read_errors->len;
  return count;
}
