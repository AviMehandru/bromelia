/* bro-listing-builder.c */
#include "bro-listing-builder.h"
#include "bro-attribute-id.h"
#include "bro-duration.h"
#include "bro-robot-private.h"

#include <stdlib.h>
#include <string.h>

struct _BroListingBuilder {
  int reported_title_count;
  GHashTable *disc;   /* id → value */
  GHashTable *titles; /* title → GHashTable (id → value) */
  GHashTable *tracks; /* title → GHashTable (stream → GHashTable (id → value)) */
};

static GHashTable *
attributes_new (void)
{
  return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}

static GHashTable *
attributes_copy (GHashTable *a)
{
  GHashTable *c = attributes_new ();
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, a);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_insert (c, k, g_strdup (v));
  return c;
}

BroListingBuilder *
bro_listing_builder_new (void)
{
  BroListingBuilder *b = g_new0 (BroListingBuilder, 1);
  b->disc = attributes_new ();
  b->titles = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);
  b->tracks = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);
  return b;
}

void
bro_listing_builder_free (BroListingBuilder *b)
{
  if (!b)
    return;
  g_hash_table_unref (b->disc);
  g_hash_table_unref (b->titles);
  g_hash_table_unref (b->tracks);
  g_free (b);
}

static GHashTable *
title_attributes (BroListingBuilder *b, int title)
{
  GHashTable *a = g_hash_table_lookup (b->titles, GINT_TO_POINTER (title));
  if (!a) {
    a = attributes_new ();
    g_hash_table_insert (b->titles, GINT_TO_POINTER (title), a);
    g_hash_table_insert (b->tracks, GINT_TO_POINTER (title),
                         g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref));
  }
  return a;
}

void
bro_listing_builder_feed (BroListingBuilder *b, const BroRobotEvent *e)
{
  switch (e->kind) {
  case BRO_ROBOT_EVENT_TITLE_COUNT:
    b->reported_title_count = e->count;
    break;
  case BRO_ROBOT_EVENT_DISC_INFO:
    g_hash_table_insert (b->disc, GINT_TO_POINTER (e->id), g_strdup (e->value));
    break;
  case BRO_ROBOT_EVENT_TITLE_INFO:
    g_hash_table_insert (title_attributes (b, e->title), GINT_TO_POINTER (e->id), g_strdup (e->value));
    break;
  case BRO_ROBOT_EVENT_STREAM_INFO: {
    title_attributes (b, e->title);
    GHashTable *tracks = g_hash_table_lookup (b->tracks, GINT_TO_POINTER (e->title));
    GHashTable *a = g_hash_table_lookup (tracks, GINT_TO_POINTER (e->stream));
    if (!a) {
      a = attributes_new ();
      g_hash_table_insert (tracks, GINT_TO_POINTER (e->stream), a);
    }
    g_hash_table_insert (a, GINT_TO_POINTER (e->id), g_strdup (e->value));
    break;
  }
  default:
    break;
  }
}

static const char *
get (GHashTable *a, BroAttributeId id)
{
  return g_hash_table_lookup (a, GINT_TO_POINTER (id));
}

static char *
dup_or_empty (const char *s)
{
  return g_strdup (s ? s : "");
}

static gboolean
int_of (const char *s, int *out)
{
  return s && _bro_robot_parse_int (s, out);
}

static gboolean
int64_of (const char *s, gint64 *out)
{
  if (!s)
    return FALSE;
  while (*s == ' ' || *s == '\t')
    s++;
  const char *p = s;
  if (*p == '-' || *p == '+')
    p++;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  while (g_ascii_isdigit (*p))
    p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p)
    return FALSE;
  *out = g_ascii_strtoll (s, NULL, 10);
  return TRUE;
}

static int
compare_ints (gconstpointer a, gconstpointer b)
{
  int x = GPOINTER_TO_INT (*(gpointer *) a), y = GPOINTER_TO_INT (*(gpointer *) b);
  return x < y ? -1 : x > y;
}

/* The keys of @table, sorted. */
static GPtrArray *
sorted_keys (GHashTable *table)
{
  GPtrArray *keys = g_ptr_array_new ();
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, table);
  while (g_hash_table_iter_next (&it, &k, NULL))
    g_ptr_array_add (keys, k);
  g_ptr_array_sort (keys, compare_ints);
  return keys;
}

static BroTrackKind
kind_of (const char *type)
{
  if (!type)
    return BRO_TRACK_KIND_UNKNOWN;
  g_autofree char *t = g_ascii_strdown (type, -1);
  if (strcmp (t, "video") == 0)
    return BRO_TRACK_KIND_VIDEO;
  if (strcmp (t, "audio") == 0)
    return BRO_TRACK_KIND_AUDIO;
  if (strcmp (t, "subtitles") == 0 || strcmp (t, "subtitle") == 0)
    return BRO_TRACK_KIND_SUBTITLE;
  if (strcmp (t, "attachment") == 0)
    return BRO_TRACK_KIND_ATTACHMENT;
  return BRO_TRACK_KIND_UNKNOWN;
}

static BroDiscType
type_of (const char *type_text)
{
  g_autofree char *t = g_ascii_strdown (type_text, -1);
  if (strstr (t, "blu"))
    return BRO_DISC_TYPE_BD;
  if (strstr (t, "hd"))
    return BRO_DISC_TYPE_HDDVD;
  if (strstr (t, "dvd"))
    return BRO_DISC_TYPE_DVD;
  return BRO_DISC_TYPE_DISC;
}

static BroTrack *
build_track (int index, GHashTable *a)
{
  BroTrack *t = g_new0 (BroTrack, 1);
  t->index = index;
  t->kind = kind_of (get (a, BRO_ATTRIBUTE_ID_TYPE));
  const char *codec = get (a, BRO_ATTRIBUTE_ID_CODEC_SHORT);
  t->codec = dup_or_empty (codec ? codec : get (a, BRO_ATTRIBUTE_ID_CODEC_ID));
  t->language = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_LANG_CODE));
  t->language_name = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_LANG_NAME));
  t->name = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_NAME));
  const char *flags = get (a, BRO_ATTRIBUTE_ID_MKV_FLAGS);
  t->is_default = flags && strchr (flags, 'd');
  t->attributes = attributes_copy (a);
  return t;
}

static BroTitle *
build_title (int index, GHashTable *a, GHashTable *tracks)
{
  BroTitle *t = g_new0 (BroTitle, 1);
  t->index = index;
  t->has_source_title_id = int_of (get (a, BRO_ATTRIBUTE_ID_ORIGINAL_TITLE_ID), &t->source_title_id);
  t->source_file = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_SOURCE_FILE_NAME));
  t->name = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_NAME));
  t->comment = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_COMMENT));
  t->duration = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_DURATION));
  BroDuration d;
  t->duration_seconds = bro_duration_parse_clock (t->duration, &d) ? (int) d.seconds : 0;
  if (!int_of (get (a, BRO_ATTRIBUTE_ID_CHAPTER_COUNT), &t->chapters))
    t->chapters = 0;
  if (!int64_of (get (a, BRO_ATTRIBUTE_ID_DISK_SIZE_BYTES), &t->size_bytes))
    t->size_bytes = 0;
  t->segment_map = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_SEGMENTS_MAP));
  t->output_file_name = dup_or_empty (get (a, BRO_ATTRIBUTE_ID_OUTPUT_FILE_NAME));
  t->has_angle = int_of (get (a, BRO_ATTRIBUTE_ID_ANGLE_INFO), &t->angle);
  t->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_track_free);
  g_autoptr (GPtrArray) keys = sorted_keys (tracks);
  for (guint i = 0; i < keys->len; i++)
    g_ptr_array_add (t->tracks, build_track (GPOINTER_TO_INT (keys->pdata[i]), g_hash_table_lookup (tracks, keys->pdata[i])));
  t->attributes = attributes_copy (a);
  return t;
}

BroListing *
bro_listing_builder_build (BroListingBuilder *b)
{
  BroListing *l = g_atomic_rc_box_new0 (BroListing);
  const char *name = get (b->disc, BRO_ATTRIBUTE_ID_NAME);
  l->name = dup_or_empty (name ? name : get (b->disc, BRO_ATTRIBUTE_ID_VOLUME_NAME));
  l->volume_name = dup_or_empty (get (b->disc, BRO_ATTRIBUTE_ID_VOLUME_NAME));
  l->type_text = dup_or_empty (get (b->disc, BRO_ATTRIBUTE_ID_TYPE));
  l->type = type_of (l->type_text);
  l->reported_title_count = b->reported_title_count;
  l->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_title_free);
  g_autoptr (GPtrArray) keys = sorted_keys (b->titles);
  for (guint i = 0; i < keys->len; i++)
    g_ptr_array_add (l->titles, build_title (GPOINTER_TO_INT (keys->pdata[i]), g_hash_table_lookup (b->titles, keys->pdata[i]),
                                             g_hash_table_lookup (b->tracks, keys->pdata[i])));
  l->attributes = attributes_copy (b->disc);
  return l;
}
