/* bro-library-marker.c */
#include "bro-library-marker.h"
#include "bro-json-value.h"

void
bro_library_marker_free (BroLibraryMarker *m)
{
  if (!m)
    return;
  g_free (m->id);
  g_free (m->name);
  g_free (m);
}

BroLibraryMarker *
bro_library_marker_make (const char *library_id, const char *path, const char *name, BroInstant created)
{
  g_autofree char *when = bro_instant_format (created);
  g_autofree char *text = g_strdup_printf ("%s\n%s\n%s", library_id, path, when);
  g_autoptr (GChecksum) sum = g_checksum_new (G_CHECKSUM_SHA256);
  guint8 b[32];
  gsize n = sizeof b;
  GString *id = g_string_new (NULL);
  BroLibraryMarker *m = g_new0 (BroLibraryMarker, 1);
  g_checksum_update (sum, (const guchar *) text, -1);
  g_checksum_get_digest (sum, b, &n);
  b[6] = (b[6] & 0x0F) | 0x80;
  b[8] = (b[8] & 0x3F) | 0x80;
  for (int i = 0; i < 16; i++)
    {
      if (i == 4 || i == 6 || i == 8 || i == 10)
        g_string_append_c (id, '-');
      g_string_append_printf (id, "%02x", b[i]);
    }
  m->id = g_string_free (id, FALSE);
  m->name = g_strdup (name);
  m->has_created = TRUE;
  m->created = created;
  return m;
}

GBytes *
bro_library_marker_encode (const BroLibraryMarker *m)
{
  g_autoptr (BroJsonValue) doc = bro_json_value_new_object ();
  bro_json_value_set (doc, "id", bro_json_value_new_string (m->id));
  bro_json_value_set (doc, "name", bro_json_value_new_string (m->name));
  if (m->has_created)
    {
      g_autofree char *when = bro_instant_format (m->created);
      bro_json_value_set (doc, "created", bro_json_value_new_string (when));
    }
  return bro_json_value_encode_canonical (doc);
}

BroLibraryMarker *
bro_library_marker_decode (GBytes *bytes)
{
  gsize size;
  const char *text = g_bytes_get_data (bytes, &size);
  g_autoptr (BroJsonValue) json = bro_json_value_parse (text, (gssize) size);
  const char *id;
  const char *created;
  BroLibraryMarker *m;
  if (!json || json->kind != BRO_JSON_VALUE_OBJECT)
    return NULL;
  id = bro_json_value_get_string (bro_json_value_member (json, "id"), NULL);
  if (!id)
    return NULL;
  m = g_new0 (BroLibraryMarker, 1);
  m->id = g_strdup (id);
  m->name = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "name"), ""));
  created = bro_json_value_get_string (bro_json_value_member (json, "created"), NULL);
  m->has_created = created && bro_instant_parse (created, &m->created);
  return m;
}
