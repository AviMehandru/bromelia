/* bro-mkv-probe.c */
#include "bro-mkv-probe.h"

#include "bro-json-value.h"

#define M bro_json_value_member

BroMkvProbe *
bro_mkv_probe_new (void)
{
  BroMkvProbe *p = g_new0 (BroMkvProbe, 1);
  p->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_mkv_track_free);
  return p;
}

void
bro_mkv_probe_free (BroMkvProbe *probe)
{
  if (!probe)
    return;
  g_ptr_array_unref (probe->tracks);
  g_free (probe);
}

BroMkvProbe *
bro_mkv_probe_parse (const char *json)
{
  g_autoptr (BroJsonValue) root = bro_json_value_parse (json, -1);
  BroJsonValue *container = M (root, "container");
  if (!container || container->kind != BRO_JSON_VALUE_OBJECT)
    return NULL;
  BroJsonValue *recognized = M (container, "recognized");
  if (recognized && recognized->kind == BRO_JSON_VALUE_BOOL && !recognized->boolean)
    return NULL;
  BroMkvProbe *p = bro_mkv_probe_new ();
  BroJsonValue *duration = M (M (container, "properties"), "duration");
  if (duration && (duration->kind == BRO_JSON_VALUE_INTEGER || duration->kind == BRO_JSON_VALUE_NUMBER)) {
    p->has_duration = TRUE;
    p->duration_seconds = bro_json_value_get_number (duration, 0) / 1e9;
  }
  BroJsonValue *tracks = M (root, "tracks");
  for (guint i = 0; i < bro_json_value_length (tracks); i++) {
    BroJsonValue *t = bro_json_value_at (tracks, i);
    g_ptr_array_add (p->tracks, bro_mkv_track_new ((int) bro_json_value_get_integer (M (t, "id"), 0),
                                                   bro_json_value_get_string (M (t, "type"), "")));
  }
  BroJsonValue *chapters = M (root, "chapters");
  for (guint i = 0; i < bro_json_value_length (chapters); i++)
    p->chapter_count += (int) bro_json_value_get_integer (M (bro_json_value_at (chapters, i), "num_entries"), 0);
  return p;
}
