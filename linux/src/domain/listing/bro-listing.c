/* bro-listing.c */
#include "bro-listing.h"

#include <string.h>

BroListing *
bro_listing_ref (BroListing *listing)
{
  return g_atomic_rc_box_acquire (listing);
}

static void
clear_listing (BroListing *l)
{
  g_free (l->name);
  g_free (l->volume_name);
  g_free (l->type_text);
  g_ptr_array_unref (l->titles);
  g_hash_table_unref (l->attributes);
}

void
bro_listing_unref (BroListing *listing)
{
  if (listing)
    g_atomic_rc_box_release_full (listing, (GDestroyNotify) clear_listing);
}

static void
put (BroJsonValue *o, const char *key, BroJsonValue *value)
{
  bro_json_value_set (o, key, value);
}

static int
compare_keys (gconstpointer a, gconstpointer b)
{
  int x = GPOINTER_TO_INT (*(gpointer *) a), y = GPOINTER_TO_INT (*(gpointer *) b);
  return x < y ? -1 : x > y;
}

static BroJsonValue *
attributes_json (GHashTable *a)
{
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, a);
  while (g_hash_table_iter_next (&it, &k, NULL))
    g_ptr_array_add (keys, k);
  g_ptr_array_sort (keys, compare_keys);
  BroJsonValue *o = bro_json_value_new_object ();
  for (guint i = 0; i < keys->len; i++) {
    g_autofree char *key = g_strdup_printf ("%d", GPOINTER_TO_INT (keys->pdata[i]));
    put (o, key, bro_json_value_new_string (g_hash_table_lookup (a, keys->pdata[i])));
  }
  return o;
}

static BroJsonValue *
int_or_null (gboolean has, int v)
{
  return has ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

BroJsonValue *
bro_listing_encode (const BroListing *l)
{
  BroJsonValue *o = bro_json_value_new_object ();
  put (o, "name", bro_json_value_new_string (l->name));
  put (o, "volumeName", bro_json_value_new_string (l->volume_name));
  put (o, "type", bro_json_value_new_string (bro_disc_type_to_wire (l->type)));
  put (o, "typeText", bro_json_value_new_string (l->type_text));
  put (o, "reportedTitleCount", bro_json_value_new_integer (l->reported_title_count));
  BroJsonValue *titles = bro_json_value_new_array ();
  for (guint i = 0; i < l->titles->len; i++) {
    const BroTitle *t = l->titles->pdata[i];
    BroJsonValue *j = bro_json_value_new_object ();
    put (j, "index", bro_json_value_new_integer (t->index));
    put (j, "sourceTitleId", int_or_null (t->has_source_title_id, t->source_title_id));
    put (j, "sourceFile", bro_json_value_new_string (t->source_file));
    put (j, "name", bro_json_value_new_string (t->name));
    put (j, "comment", bro_json_value_new_string (t->comment));
    put (j, "duration", bro_json_value_new_string (t->duration));
    put (j, "durationSeconds", bro_json_value_new_integer (t->duration_seconds));
    put (j, "chapters", bro_json_value_new_integer (t->chapters));
    put (j, "sizeBytes", bro_json_value_new_integer (t->size_bytes));
    put (j, "segmentMap", bro_json_value_new_string (t->segment_map));
    put (j, "outputFileName", bro_json_value_new_string (t->output_file_name));
    put (j, "angle", int_or_null (t->has_angle, t->angle));
    BroJsonValue *tracks = bro_json_value_new_array ();
    for (guint k = 0; k < t->tracks->len; k++) {
      const BroTrack *x = t->tracks->pdata[k];
      BroJsonValue *tj = bro_json_value_new_object ();
      put (tj, "index", bro_json_value_new_integer (x->index));
      put (tj, "kind", bro_json_value_new_string (bro_track_kind_to_wire (x->kind)));
      put (tj, "codec", bro_json_value_new_string (x->codec));
      put (tj, "language", bro_json_value_new_string (x->language));
      put (tj, "languageName", bro_json_value_new_string (x->language_name));
      put (tj, "name", bro_json_value_new_string (x->name));
      put (tj, "isDefault", bro_json_value_new_bool (x->is_default));
      put (tj, "attributes", attributes_json (x->attributes));
      bro_json_value_append (tracks, tj);
    }
    put (j, "tracks", tracks);
    put (j, "attributes", attributes_json (t->attributes));
    bro_json_value_append (titles, j);
  }
  put (o, "titles", titles);
  put (o, "attributes", attributes_json (l->attributes));
  return o;
}

static char *
string_of (const BroJsonValue *json, const char *key)
{
  return g_strdup (bro_json_value_get_string (bro_json_value_member (json, key), ""));
}

static int
int_of (const BroJsonValue *json, const char *key)
{
  return (int) bro_json_value_get_integer (bro_json_value_member (json, key), 0);
}

/* TRUE and @out when the member is a number. */
static gboolean
optional_int (const BroJsonValue *json, const char *key, int *out)
{
  BroJsonValue *v = bro_json_value_member (json, key);
  if (!v || (v->kind != BRO_JSON_VALUE_INTEGER && v->kind != BRO_JSON_VALUE_NUMBER)) {
    *out = 0;
    return FALSE;
  }
  *out = (int) bro_json_value_get_integer (v, 0);
  return TRUE;
}

/* Keys that are only digits; values that are strings. */
static GHashTable *
attributes_of (const BroJsonValue *json)
{
  GHashTable *a = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  if (!json || json->kind != BRO_JSON_VALUE_OBJECT)
    return a;
  for (guint i = 0; i < json->keys->len; i++) {
    const char *key = json->keys->pdata[i];
    const BroJsonValue *v = json->items->pdata[i];
    gboolean digits = *key != '\0';
    for (const char *c = key; *c; c++)
      digits = digits && g_ascii_isdigit (*c);
    gint64 id = digits && strlen (key) <= 10 ? g_ascii_strtoll (key, NULL, 10) : -1;
    if (id >= 0 && id <= G_MAXINT32 && v->kind == BRO_JSON_VALUE_STRING)
      g_hash_table_insert (a, GINT_TO_POINTER ((int) id), g_strdup (v->string));
  }
  return a;
}

BroListing *
bro_listing_decode (const BroJsonValue *json)
{
  BroListing *l = g_atomic_rc_box_new0 (BroListing);
  l->name = string_of (json, "name");
  l->volume_name = string_of (json, "volumeName");
  if (!bro_disc_type_from_wire (bro_json_value_get_string (bro_json_value_member (json, "type"), ""), &l->type))
    l->type = BRO_DISC_TYPE_DISC;
  l->type_text = string_of (json, "typeText");
  l->reported_title_count = int_of (json, "reportedTitleCount");
  l->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_title_free);
  const BroJsonValue *titles = bro_json_value_member (json, "titles");
  for (guint i = 0; titles && titles->kind == BRO_JSON_VALUE_ARRAY && i < titles->items->len; i++) {
    const BroJsonValue *tj = titles->items->pdata[i];
    BroTitle *t = g_new0 (BroTitle, 1);
    t->index = int_of (tj, "index");
    t->has_source_title_id = optional_int (tj, "sourceTitleId", &t->source_title_id);
    t->source_file = string_of (tj, "sourceFile");
    t->name = string_of (tj, "name");
    t->comment = string_of (tj, "comment");
    t->duration = string_of (tj, "duration");
    t->duration_seconds = int_of (tj, "durationSeconds");
    t->chapters = int_of (tj, "chapters");
    t->size_bytes = bro_json_value_get_integer (bro_json_value_member (tj, "sizeBytes"), 0);
    t->segment_map = string_of (tj, "segmentMap");
    t->output_file_name = string_of (tj, "outputFileName");
    t->has_angle = optional_int (tj, "angle", &t->angle);
    t->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_track_free);
    const BroJsonValue *tracks = bro_json_value_member (tj, "tracks");
    for (guint k = 0; tracks && tracks->kind == BRO_JSON_VALUE_ARRAY && k < tracks->items->len; k++) {
      const BroJsonValue *kj = tracks->items->pdata[k];
      BroTrack *x = g_new0 (BroTrack, 1);
      x->index = int_of (kj, "index");
      if (!bro_track_kind_from_wire (bro_json_value_get_string (bro_json_value_member (kj, "kind"), ""), &x->kind))
        x->kind = BRO_TRACK_KIND_UNKNOWN;
      x->codec = string_of (kj, "codec");
      x->language = string_of (kj, "language");
      x->language_name = string_of (kj, "languageName");
      x->name = string_of (kj, "name");
      x->is_default = bro_json_value_get_bool (bro_json_value_member (kj, "isDefault"), FALSE);
      x->attributes = attributes_of (bro_json_value_member (kj, "attributes"));
      g_ptr_array_add (t->tracks, x);
    }
    t->attributes = attributes_of (bro_json_value_member (tj, "attributes"));
    g_ptr_array_add (l->titles, t);
  }
  l->attributes = attributes_of (bro_json_value_member (json, "attributes"));
  return l;
}
