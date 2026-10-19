/* bro-acquired-title.c */
#include "bro-acquired-title.h"

BroAcquiredTitle *
bro_acquired_title_new (int index)
{
  BroAcquiredTitle *t = g_new0 (BroAcquiredTitle, 1);
  t->index = index;
  t->files = g_ptr_array_new_with_free_func (g_free);
  t->read_errors = g_ptr_array_new_with_free_func (g_free);
  return t;
}

BroAcquiredTitle *
bro_acquired_title_copy (const BroAcquiredTitle *title)
{
  BroAcquiredTitle *t = bro_acquired_title_new (title->index);
  for (guint i = 0; i < title->files->len; i++)
    g_ptr_array_add (t->files, g_strdup (title->files->pdata[i]));
  for (guint i = 0; i < title->read_errors->len; i++)
    g_ptr_array_add (t->read_errors, g_strdup (title->read_errors->pdata[i]));
  return t;
}

void
bro_acquired_title_free (BroAcquiredTitle *title)
{
  if (!title)
    return;
  g_ptr_array_unref (title->files);
  g_ptr_array_unref (title->read_errors);
  g_free (title);
}

static BroJsonValue *
strings (GPtrArray *list)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < list->len; i++)
    bro_json_value_append (a, bro_json_value_new_string (list->pdata[i]));
  return a;
}

BroJsonValue *
bro_acquired_title_encode (const BroAcquiredTitle *title)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "index", bro_json_value_new_integer (title->index));
  bro_json_value_set (o, "files", strings (title->files));
  bro_json_value_set (o, "readErrors", strings (title->read_errors));
  return o;
}

BroAcquiredTitle *
bro_acquired_title_decode (const BroJsonValue *json)
{
  BroAcquiredTitle *t = bro_acquired_title_new ((int) bro_json_value_get_integer (bro_json_value_member (json, "index"), 0));
  BroJsonValue *files = bro_json_value_member (json, "files");
  BroJsonValue *errors = bro_json_value_member (json, "readErrors");
  for (guint i = 0; i < bro_json_value_length (files); i++)
    g_ptr_array_add (t->files, g_strdup (bro_json_value_get_string (bro_json_value_at (files, i), "")));
  for (guint i = 0; i < bro_json_value_length (errors); i++)
    g_ptr_array_add (t->read_errors, g_strdup (bro_json_value_get_string (bro_json_value_at (errors, i), "")));
  return t;
}
