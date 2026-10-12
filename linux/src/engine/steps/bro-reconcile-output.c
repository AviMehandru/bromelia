/* bro-reconcile-output.c */
#include "bro-reconcile-output.h"

BroReconcileOutput *
bro_reconcile_output_new (GHashTable *title_map)
{
  BroReconcileOutput *o = g_new0 (BroReconcileOutput, 1);
  o->title_map = title_map ? title_map : g_hash_table_new (g_direct_hash, g_direct_equal);
  return o;
}

void
bro_reconcile_output_free (BroReconcileOutput *output)
{
  if (!output)
    return;
  g_hash_table_unref (output->title_map);
  g_free (output);
}

static int
compare_ints (gconstpointer a, gconstpointer b)
{
  int x = GPOINTER_TO_INT (*(gpointer *) a), y = GPOINTER_TO_INT (*(gpointer *) b);
  return x < y ? -1 : x > y;
}

BroJsonValue *
bro_reconcile_output_encode (const BroReconcileOutput *output)
{
  g_autoptr (GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (output->title_map);
  g_ptr_array_sort (keys, compare_ints);
  BroJsonValue *map = bro_json_value_new_array ();
  for (guint i = 0; i < keys->len; i++) {
    BroJsonValue *pair = bro_json_value_new_array ();
    bro_json_value_append (pair, bro_json_value_new_integer (GPOINTER_TO_INT (keys->pdata[i])));
    bro_json_value_append (pair, bro_json_value_new_integer (GPOINTER_TO_INT (g_hash_table_lookup (output->title_map, keys->pdata[i]))));
    bro_json_value_append (map, pair);
  }
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "titleMap", map);
  return o;
}

BroReconcileOutput *
bro_reconcile_output_decode (const BroJsonValue *json)
{
  GHashTable *map = g_hash_table_new (g_direct_hash, g_direct_equal);
  BroJsonValue *pairs = bro_json_value_member (json, "titleMap");
  for (guint i = 0; pairs && pairs->kind == BRO_JSON_VALUE_ARRAY && i < pairs->items->len; i++) {
    BroJsonValue *p = pairs->items->pdata[i];
    if (p->kind == BRO_JSON_VALUE_ARRAY && p->items->len == 2)
      g_hash_table_insert (map, GINT_TO_POINTER ((int) bro_json_value_get_integer (p->items->pdata[0], 0)),
                           GINT_TO_POINTER ((int) bro_json_value_get_integer (p->items->pdata[1], 0)));
  }
  return bro_reconcile_output_new (map);
}
