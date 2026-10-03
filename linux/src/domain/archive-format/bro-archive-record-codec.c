/* bro-archive-record-codec.c */
#include "bro-archive-record-codec.h"
#include "bro-label-parser.h"

#include <string.h>

/* The key order of each object of a version 3 record (archive-record-3.json); keys it doesn't name follow in the
 * order they had. */
static const char *const key_order[][20] = {
  { "", "format", "version", "unit", "status", "name", "kind", "work", "disc", "physical", "acquisition", "job", "titles", "episodes", "files",
    "attempts", "parity", "counts", "problems", "logs" },
  { "unit", "id", "short", "library" },
  { "work", "provider", "title", "year", "tmdbId", "imdbId", "chosen" },
  { "disc", "label", "volumeName", "type", "format", "formatCode", "encrypted", "season", "part", "volume", "disc", "set", "fingerprint",
    "libreDrive" },
  { "physical", "barcode", "location" },
  { "acquisition", "mode", "rip", "source", "drive", "profile", "rules", "makemkv", "bromelia", "tools" },
  { "acquisition.drive", "id", "name", "model" },
  { "job", "id", "startedAt", "finishedAt" },
  { "titles[]", "index", "sourceTitleId", "sourceFile", "duration", "durationSeconds", "chapters", "sizeBytes", "segmentMap", "readErrors" },
  { "episodes[]", "file", "episode", "sourceTitleId", "firstChapter", "lastChapter", "title" },
  { "files[]", "path", "size", "sha256", "role", "title" },
  { "attempts[]", "job", "drive", "startedAt", "titles", "kept" },
  { "parity", "percent", "files" },
  { "counts", "warnings", "errors" },
  { "problems[]", "code", "params", "text", "readError" },
};

static const char *const *
order_for (const char *path)
{
  for (guint i = 0; i < G_N_ELEMENTS (key_order); i++)
    if (strcmp (key_order[i][0], path) == 0)
      return key_order[i] + 1;
  return NULL;
}

static int
rank (const char *const *order, const char *key)
{
  int i = 0;
  for (; order[i]; i++)
    if (strcmp (order[i], key) == 0)
      return i;
  return i;
}

static BroJsonValue *
ordered (const BroJsonValue *v, const char *path)
{
  if (v->kind == BRO_JSON_VALUE_ARRAY) {
    BroJsonValue *a = bro_json_value_new_array ();
    g_autofree char *item_path = g_strconcat (path, "[]", NULL);
    for (guint i = 0; i < v->items->len; i++)
      bro_json_value_append (a, ordered (v->items->pdata[i], item_path));
    return a;
  }
  if (v->kind != BRO_JSON_VALUE_OBJECT)
    return bro_json_value_ref ((BroJsonValue *) v);
  const char *const *order = order_for (path);
  guint n = v->keys->len;
  g_autofree guint *index = g_new (guint, n);
  for (guint i = 0; i < n; i++)
    index[i] = i;
  /* A stable insertion sort by rank: keys the schema doesn't name keep their order. */
  for (guint i = 1; order && i < n; i++) {
    guint x = index[i];
    int r = rank (order, v->keys->pdata[x]);
    guint j = i;
    while (j > 0 && rank (order, v->keys->pdata[index[j - 1]]) > r) {
      index[j] = index[j - 1];
      j--;
    }
    index[j] = x;
  }
  BroJsonValue *o = bro_json_value_new_object ();
  for (guint i = 0; i < n; i++) {
    const char *key = v->keys->pdata[index[i]];
    g_autofree char *child = path[0] ? g_strconcat (path, ".", key, NULL) : g_strdup (key);
    bro_json_value_set (o, key, ordered (v->items->pdata[index[i]], child));
  }
  return o;
}

static int
int_of (BroJsonValue *v)
{
  return v && (v->kind == BRO_JSON_VALUE_INTEGER || v->kind == BRO_JSON_VALUE_NUMBER) ? (int) bro_json_value_get_integer (v, -1) : -1;
}

BroArchiveRecord *
bro_archive_record_codec_decode (const char *bytes, gssize length)
{
  g_autoptr (BroJsonValue) doc = bro_json_value_parse (bytes, length);
  if (!doc || doc->kind != BRO_JSON_VALUE_OBJECT || g_strcmp0 (bro_json_value_get_string (bro_json_value_member (doc, "format"), NULL), "bromelia-archive") != 0)
    return NULL;
  gint64 version = bro_json_value_get_integer (bro_json_value_member (doc, "version"), 0);
  if (version != 2 && version != 3)
    return NULL;
  BroJsonValue *disc = bro_json_value_member (doc, "disc");
  BroArchiveRecord *r = g_new0 (BroArchiveRecord, 1);
  r->version = (int) version;
  r->document = g_steal_pointer (&doc);
#define S(o, k) g_strdup (bro_json_value_get_string (bro_json_value_member ((o), (k)), ""))
  r->status = S (r->document, "status");
  r->name = S (r->document, "name");
  r->kind = S (r->document, "kind");
  r->label = S (disc, "label");
  r->volume_name = S (disc, "volumeName");
#undef S
  r->season = int_of (bro_json_value_member (disc, "season"));
  r->part = int_of (bro_json_value_member (disc, "part"));
  r->volume = int_of (bro_json_value_member (disc, "volume"));
  r->disc = int_of (bro_json_value_member (disc, "disc"));
  r->fingerprint = g_strdup (bro_json_value_get_string (bro_json_value_member (disc, "fingerprint"), NULL));
  r->unit_id = version == 3 ? g_strdup (bro_json_value_get_string (bro_json_value_member (bro_json_value_member (r->document, "unit"), "id"), NULL))
                            : NULL;
  r->episodes = g_array_new (FALSE, FALSE, sizeof (int));
  BroJsonValue *eps = bro_json_value_member (r->document, "episodes");
  for (guint i = 0; i < bro_json_value_length (eps); i++) {
    int n = int_of (bro_json_value_member (bro_json_value_at (eps, i), "episode"));
    if (n >= 0)
      g_array_append_val (r->episodes, n);
  }
  r->files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_sum_entry_free);
  BroJsonValue *files = bro_json_value_member (r->document, "files");
  for (guint i = 0; i < bro_json_value_length (files); i++) {
    BroJsonValue *f = bro_json_value_at (files, i);
    const char *path = bro_json_value_get_string (bro_json_value_member (f, "path"), NULL);
    const char *sha = bro_json_value_get_string (bro_json_value_member (f, "sha256"), NULL);
    if (path && sha)
      g_ptr_array_add (r->files, bro_sum_entry_new (path, sha));
  }
  return r;
}

GBytes *
bro_archive_record_codec_encode_v3 (const BroArchiveRecord *record)
{
  g_return_val_if_fail (record->version == 3, NULL);
  g_autoptr (BroJsonValue) doc = ordered (record->document, "");
  return bro_json_value_encode_canonical (doc);
}

BroArchivedDisc *
bro_archive_record_codec_archived_disc (const BroArchiveRecord *record, const char *folder)
{
  if ((strcmp (record->status, "success") != 0 && strcmp (record->status, "errors") != 0) || strcmp (record->kind, "tv") != 0)
    return NULL;
  g_autoptr (BroLabel) label = bro_label_parser_parse (record->volume_name[0] ? record->volume_name : record->label);
  BroArchivedDisc *d = bro_archived_disc_new (record->name, label->title, folder);
  d->season = record->season;
  d->part = record->part;
  d->volume = record->volume;
  d->disc = record->disc;
  for (guint i = 0; i < record->episodes->len; i++)
    d->last_episode = MAX (d->last_episode, g_array_index (record->episodes, int, i));
  return d;
}
