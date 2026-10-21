/* bro-named-file.c */
#include "bro-named-file.h"

BroNamedFile *
bro_named_file_new (const char *path, const char *role, int title)
{
  BroNamedFile *f = g_new0 (BroNamedFile, 1);
  f->path = g_strdup (path);
  f->role = g_strdup (role);
  f->has_title = title >= 0;
  f->title = title >= 0 ? title : 0;
  return f;
}

BroNamedFile *
bro_named_file_copy (const BroNamedFile *file)
{
  return bro_named_file_new (file->path, file->role, file->has_title ? file->title : -1);
}

void
bro_named_file_free (BroNamedFile *file)
{
  if (!file)
    return;
  g_free (file->path);
  g_free (file->role);
  g_free (file);
}

BroJsonValue *
bro_named_file_encode (const BroNamedFile *file)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "path", bro_json_value_new_string (file->path));
  if (file->role)
    bro_json_value_set (o, "role", bro_json_value_new_string (file->role));
  if (file->has_title)
    bro_json_value_set (o, "title", bro_json_value_new_integer (file->title));
  return o;
}

BroNamedFile *
bro_named_file_decode (const BroJsonValue *json)
{
  BroJsonValue *title = bro_json_value_member (json, "title");
  BroNamedFile *f = bro_named_file_new (bro_json_value_get_string (bro_json_value_member (json, "path"), ""),
                                        bro_json_value_get_string (bro_json_value_member (json, "role"), NULL), -1);
  if (title && title->kind == BRO_JSON_VALUE_INTEGER)
    {
      f->has_title = TRUE;
      f->title = (int) title->integer;
    }
  return f;
}
