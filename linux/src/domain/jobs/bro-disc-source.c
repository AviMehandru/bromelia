/* bro-disc-source.c */
#include "bro-disc-source.h"

static const char *const keys[] = { "drive", "iso", "folder", "file" };

BroDiscSource *
bro_disc_source_new (BroDiscSourceKind kind, const char *value)
{
  BroDiscSource *s = g_new0 (BroDiscSource, 1);
  s->kind = kind;
  s->value = g_strdup (value ? value : "");
  return s;
}

BroDiscSource *
bro_disc_source_copy (const BroDiscSource *source)
{
  return source ? bro_disc_source_new (source->kind, source->value) : NULL;
}

void
bro_disc_source_free (BroDiscSource *source)
{
  if (!source)
    return;
  g_free (source->value);
  g_free (source);
}

BroJsonValue *
bro_disc_source_encode (const BroDiscSource *source)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, keys[source->kind], bro_json_value_new_string (source->value));
  return o;
}

BroDiscSource *
bro_disc_source_decode (const BroJsonValue *json)
{
  for (guint k = 0; k < G_N_ELEMENTS (keys); k++) {
    BroJsonValue *v = bro_json_value_member (json, keys[k]);
    if (v && v->kind == BRO_JSON_VALUE_STRING)
      return bro_disc_source_new ((BroDiscSourceKind) k, v->string);
  }
  return NULL;
}
