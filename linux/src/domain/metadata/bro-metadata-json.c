/* bro-metadata-json.c */
#include "bro-metadata-private.h"

#include <string.h>

char *
_bro_metadata_escape (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
    if (g_ascii_isalnum (*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~')
      g_string_append_c (out, *p);
    else
      g_string_append_printf (out, "%%%02X", *p);
  }
  return g_string_free (out, FALSE);
}

BroJsonValue *
_bro_metadata_object (GBytes *bytes)
{
  gsize n = 0;
  const char *d = bytes ? g_bytes_get_data (bytes, &n) : NULL;
  BroJsonValue *v = d ? bro_json_value_parse (d, n) : NULL;
  if (v && v->kind != BRO_JSON_VALUE_OBJECT) {
    bro_json_value_unref (v);
    return NULL;
  }
  return v;
}

const char *
_bro_metadata_text (BroJsonValue *v)
{
  const char *s = bro_json_value_get_string (v, NULL);
  return s && strcmp (s, "N/A") != 0 ? s : "";
}

int
_bro_metadata_year (BroJsonValue *v)
{
  const char *s = bro_json_value_get_string (v, NULL);
  if (!s || strlen (s) < 4)
    return -1;
  for (int i = 0; i < 4; i++)
    if (!g_ascii_isdigit (s[i]))
      return -1;
  return (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

gboolean
_bro_metadata_int (BroJsonValue *v, int *out)
{
  if (!v || v->kind != BRO_JSON_VALUE_INTEGER || v->integer < G_MININT32 || v->integer > G_MAXINT32)
    return FALSE;
  *out = (int) v->integer;
  return TRUE;
}
