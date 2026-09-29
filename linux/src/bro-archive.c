/* bro-archive.c — disc fingerprints, finding earlier archives of a disc, and verifying SHA256SUMS folders again. */
#include "bro-archive.h"
#include "bro-config.h"
#include "bro-identity.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- disc fingerprints ---- */

static int
cmp_str (gconstpointer a, gconstpointer b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

char *
bro_disc_fingerprint (BroDiscInfo *info)
{
  g_autoptr (GPtrArray) lines = NULL;
  g_autoptr (GString) text = NULL;
  g_autofree char *hash = NULL;
  const char *volume;
  if (!info || !info->titles->len)
    return NULL;
  volume = bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME);
  if (!volume || !*volume)
    volume = bro_disc_info_name (info);
  lines = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      g_ptr_array_add (lines, g_strdup_printf ("%d|%d|%s|%" G_GINT64_FORMAT, bro_title_source_id (t), bro_title_duration (t),
                                               bro_title_str (t, BRO_ATTR_SEGMENTS_MAP), bro_title_size (t)));
    }
  g_ptr_array_sort (lines, cmp_str);
  text = g_string_new ("bromelia-disc-fingerprint 1\n");
  g_string_append_printf (text, "volume:%s\ntitles:%u\n", volume ? volume : "", info->titles->len);
  for (guint i = 0; i < lines->len; i++)
    g_string_append_printf (text, "%s\n", (char *) lines->pdata[i]);
  hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, text->str, text->len);
  return g_strdup_printf ("v1:%.32s", hash);
}

void
bro_archived_match_free (BroArchivedMatch *m)
{
  if (!m)
    return;
  g_free (m->folder);
  g_free (m);
}

void
bro_archived_candidate_free (BroArchivedCandidate *c)
{
  if (!c)
    return;
  g_free (c->fingerprint);
  g_free (c->folder);
  g_free (c->state);
  g_free (c);
}

static gint64
parse_time (const char *s)
{
  g_autoptr (GDateTime) d = s && *s ? g_date_time_new_from_iso8601 (s, NULL) : NULL;
  return d ? g_date_time_to_unix (d) : 0;
}

/* An archive record (bromelia*.json) for fingerprint with status "success". */
static gboolean
record_matches (const char *path, const char *fingerprint, gint64 *when)
{
  g_autoptr (JsonParser) p = json_parser_new ();
  JsonObject *o, *disc;
  if (!json_parser_load_from_file (p, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    return FALSE;
  o = json_node_get_object (json_parser_get_root (p));
  if (g_strcmp0 (json_object_get_string_member_with_default (o, "format", ""), "bromelia-archive") != 0
      || g_strcmp0 (json_object_get_string_member_with_default (o, "status", ""), "success") != 0
      || !json_object_has_member (o, "disc") || !JSON_NODE_HOLDS_OBJECT (json_object_get_member (o, "disc")))
    return FALSE;
  disc = json_object_get_object_member (o, "disc");
  if (g_strcmp0 (json_object_get_string_member_with_default (disc, "fingerprint", ""), fingerprint) != 0)
    return FALSE;
  *when = parse_time (json_object_get_string_member_with_default (o, "finishedAt", ""));
  return TRUE;
}

static gboolean
is_record_name (const char *name)
{
  return g_str_has_prefix (name, "bromelia") && g_str_has_suffix (name, ".json");
}

static BroArchivedMatch *
scan_for_record (const char *dir, const char *fingerprint, int depth)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) subdirs = g_ptr_array_new_with_free_func (g_free);
  const char *name;
  gint64 when = 0;
  if (!d)
    return NULL;
  while ((name = g_dir_read_name (d)))
    {
      g_autofree char *full = NULL;
      if (name[0] == '.')
        continue;
      full = g_build_filename (dir, name, NULL);
      if (is_record_name (name) && g_file_test (full, G_FILE_TEST_IS_REGULAR))
        {
          if (record_matches (full, fingerprint, &when))
            {
              BroArchivedMatch *m = g_new0 (BroArchivedMatch, 1);
              m->folder = g_strdup (dir);
              m->archived_at = when;
              return m;
            }
        }
      else if (depth < 4 && g_file_test (full, G_FILE_TEST_IS_DIR) && !g_file_test (full, G_FILE_TEST_IS_SYMLINK))
        g_ptr_array_add (subdirs, g_steal_pointer (&full));
    }
  g_ptr_array_sort (subdirs, cmp_str);
  for (guint i = 0; i < subdirs->len; i++)
    {
      BroArchivedMatch *m = scan_for_record (subdirs->pdata[i], fingerprint, depth + 1);
      if (m)
        return m;
    }
  return NULL;
}

BroArchivedMatch *
bro_find_archived (const char *fingerprint, GPtrArray *candidates, const char *root)
{
  if (!fingerprint || !*fingerprint)
    return NULL;
  for (guint i = 0; candidates && i < candidates->len; i++)
    {
      BroArchivedCandidate *c = candidates->pdata[i];
      if (g_strcmp0 (c->fingerprint, fingerprint) == 0 && g_strcmp0 (c->state, "success") == 0 && c->folder
          && g_file_test (c->folder, G_FILE_TEST_IS_DIR))
        {
          BroArchivedMatch *m = g_new0 (BroArchivedMatch, 1);
          m->folder = g_strdup (c->folder);
          m->archived_at = c->finished_at;
          return m;
        }
    }
  return root && *root ? scan_for_record (root, fingerprint, 0) : NULL;
}

/* ---- verifying archives ---- */

void
bro_folder_check_free (BroFolderCheck *r)
{
  if (!r)
    return;
  g_free (r->folder);
  g_ptr_array_unref (r->missing);
  g_ptr_array_unref (r->changed);
  g_ptr_array_unref (r->unreadable);
  g_ptr_array_unref (r->extra);
  g_free (r->error);
  g_free (r);
}

gboolean
bro_folder_check_ok (const BroFolderCheck *r)
{
  return !r->error && !r->missing->len && !r->changed->len && !r->unreadable->len;
}

char *
bro_folder_check_summary (const BroFolderCheck *r)
{
  GString *s;
  if (r->error)
    return g_strdup (r->error);
  s = g_string_new (NULL);
  if (bro_folder_check_ok (r))
    g_string_append_printf (s, "%d file(s) OK", r->files);
  else
    {
      if (r->changed->len)
        g_string_append_printf (s, "%u changed", r->changed->len);
      if (r->unreadable->len)
        g_string_append_printf (s, "%s%u unreadable", s->len ? ", " : "", r->unreadable->len);
      if (r->missing->len)
        g_string_append_printf (s, "%s%u missing", s->len ? ", " : "", r->missing->len);
      g_string_append_printf (s, " of %d file(s)", r->files);
    }
  if (r->extra->len)
    g_string_append_printf (s, "; %u not listed", r->extra->len);
  return g_string_free (s, FALSE);
}

static void
collect_folders (const char *dir, GPtrArray *out)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autofree char *sums = g_build_filename (dir, BRO_CHECKSUM_FILE, NULL);
  const char *name;
  if (!d)
    return;
  if (g_file_test (sums, G_FILE_TEST_IS_REGULAR))
    g_ptr_array_add (out, g_strdup (dir));
  while ((name = g_dir_read_name (d)))
    {
      g_autofree char *full = NULL;
      if (name[0] == '.')
        continue;
      full = g_build_filename (dir, name, NULL);
      if (g_file_test (full, G_FILE_TEST_IS_DIR) && !g_file_test (full, G_FILE_TEST_IS_SYMLINK))
        collect_folders (full, out);
    }
}

GPtrArray *
bro_archive_folders (const char *path)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  collect_folders (path, out);
  g_ptr_array_sort (out, cmp_str);
  return out;
}

/* Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list. */
static gboolean
own_file (const char *name)
{
  return g_str_equal (name, BRO_CHECKSUM_FILE) || is_record_name (name)
         || (g_str_has_prefix (name, "bromelia-log") && g_str_has_suffix (name, ".txt"))
         || g_str_equal (name, "INCOMPLETE.txt") || g_str_equal (name, "READ ERRORS.txt");
}

/* Visible files under dir (relative to base) that aren't listed, skipping folders that are archives of their own. */
static void
collect_extra (const char *base, const char *rel, GHashTable *listed, GPtrArray *extra)
{
  g_autofree char *dir = *rel ? g_build_filename (base, rel, NULL) : g_strdup (base);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *name;
  if (!d)
    return;
  while ((name = g_dir_read_name (d)))
    if (name[0] != '.')
      g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort (names, cmp_str);
  for (guint i = 0; i < names->len; i++)
    {
      const char *n = names->pdata[i];
      g_autofree char *r = *rel ? g_strconcat (rel, "/", n, NULL) : g_strdup (n);
      g_autofree char *full = g_build_filename (base, r, NULL);
      if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
          g_autofree char *sums = g_build_filename (full, BRO_CHECKSUM_FILE, NULL);
          if (!g_file_test (sums, G_FILE_TEST_EXISTS) && !g_file_test (full, G_FILE_TEST_IS_SYMLINK))
            collect_extra (base, r, listed, extra);
        }
      else if (!g_hash_table_contains (listed, r) && !(*rel == '\0' && own_file (n)))
        g_ptr_array_add (extra, g_steal_pointer (&r));
    }
}

typedef struct {
  BroVerifyProgress progress;
  gpointer user_data;
  gint64 before, total;
  const char *folder, *file;
  gboolean stopped;
} HashCtx;

static gboolean
hash_progress (gint64 done, gpointer data)
{
  HashCtx *h = data;
  if (h->progress && !h->progress (h->before + done, h->total, h->folder, h->file, h->user_data))
    h->stopped = TRUE;
  return !h->stopped;
}

static BroFolderCheck *
check_folder (const char *folder, GHashTable *sums, HashCtx *h)
{
  BroFolderCheck *r = g_new0 (BroFolderCheck, 1);
  g_autoptr (GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (sums);
  r->folder = g_strdup (folder);
  r->missing = g_ptr_array_new_with_free_func (g_free);
  r->changed = g_ptr_array_new_with_free_func (g_free);
  r->unreadable = g_ptr_array_new_with_free_func (g_free);
  r->extra = g_ptr_array_new_with_free_func (g_free);
  r->files = g_hash_table_size (sums);
  g_ptr_array_sort (keys, cmp_str);
  h->folder = folder;
  for (guint i = 0; i < keys->len && !h->stopped; i++)
    {
      const char *rel = keys->pdata[i];
      g_autofree char *full = g_build_filename (folder, rel, NULL);
      g_autoptr (GError) err = NULL;
      g_autofree char *hash = NULL;
      struct stat st;
      if (!g_file_test (full, G_FILE_TEST_IS_REGULAR) || g_stat (full, &st) != 0)
        {
          g_ptr_array_add (r->missing, g_strdup (rel));
          continue;
        }
      h->file = rel;
      hash = bro_sha256_file (full, hash_progress, h, &err);
      h->before += st.st_size;
      r->bytes += st.st_size;
      if (h->stopped)
        break;
      if (!hash)
        g_ptr_array_add (r->unreadable, g_strdup (rel));
      else if (!g_str_equal (hash, g_hash_table_lookup (sums, rel)))
        g_ptr_array_add (r->changed, g_strdup (rel));
    }
  if (h->stopped)
    {
      bro_folder_check_free (r);
      return NULL;
    }
  collect_extra (folder, "", sums, r->extra);
  return r;
}

GPtrArray *
bro_verify_folders (GPtrArray *folders, BroVerifyProgress progress, gpointer user_data, gboolean *stopped)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_folder_check_free);
  g_autoptr (GPtrArray) all_sums = g_ptr_array_new_with_free_func ((GDestroyNotify) g_hash_table_unref);
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  HashCtx h = { progress, user_data, 0, 0, NULL, NULL, FALSE };

  /* Read every SHA256SUMS first, for the total. */
  for (guint i = 0; i < folders->len; i++)
    {
      g_autofree char *path = g_build_filename (folders->pdata[i], BRO_CHECKSUM_FILE, NULL);
      g_autofree char *text = NULL;
      g_autoptr (GError) err = NULL;
      GHashTable *sums = NULL;
      if (g_file_get_contents (path, &text, NULL, &err))
        sums = bro_checksums_parse (text);
      g_ptr_array_add (errors, sums ? (g_hash_table_size (sums) ? NULL : g_strdup ("SHA256SUMS lists no files"))
                                    : g_strdup_printf ("SHA256SUMS can't be read: %s", err->message));
      if (!sums)
        sums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_ptr_array_add (all_sums, sums);
      {
        GHashTableIter it;
        gpointer k;
        g_hash_table_iter_init (&it, sums);
        while (g_hash_table_iter_next (&it, &k, NULL))
          {
            g_autofree char *full = g_build_filename (folders->pdata[i], k, NULL);
            struct stat st;
            if (g_stat (full, &st) == 0)
              h.total += st.st_size;
          }
      }
    }
  if (progress && !progress (0, h.total, NULL, NULL, user_data))
    h.stopped = TRUE;
  for (guint i = 0; i < folders->len && !h.stopped; i++)
    {
      BroFolderCheck *r;
      if (errors->pdata[i])
        {
          r = g_new0 (BroFolderCheck, 1);
          r->folder = g_strdup (folders->pdata[i]);
          r->missing = g_ptr_array_new_with_free_func (g_free);
          r->changed = g_ptr_array_new_with_free_func (g_free);
          r->unreadable = g_ptr_array_new_with_free_func (g_free);
          r->extra = g_ptr_array_new_with_free_func (g_free);
          r->error = g_strdup (errors->pdata[i]);
        }
      else if (!(r = check_folder (folders->pdata[i], all_sums->pdata[i], &h)))
        break;
      g_ptr_array_add (out, r);
    }
  if (stopped)
    *stopped = h.stopped;
  return out;
}

/* ---- check records ---- */

void
bro_check_record_free (BroCheckRecord *r)
{
  if (!r)
    return;
  g_free (r->summary);
  g_free (r);
}

GHashTable *
bro_check_records_load (const char *path)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_check_record_free);
  g_autoptr (JsonParser) p = json_parser_new ();
  JsonObject *o;
  GList *members;
  if (!json_parser_load_from_file (p, path, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (p)))
    return t;
  o = json_node_get_object (json_parser_get_root (p));
  members = json_object_get_members (o);
  for (GList *l = members; l; l = l->next)
    {
      JsonNode *n = json_object_get_member (o, l->data);
      JsonObject *e;
      BroCheckRecord *r;
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      e = json_node_get_object (n);
      r = g_new0 (BroCheckRecord, 1);
      r->checked_at = json_object_get_int_member_with_default (e, "checkedAt", 0);
      r->ok = json_object_get_boolean_member_with_default (e, "ok", FALSE);
      r->summary = g_strdup (json_object_get_string_member_with_default (e, "summary", ""));
      g_hash_table_replace (t, g_strdup (l->data), r);
    }
  g_list_free (members);
  return t;
}

gboolean
bro_check_records_save (GHashTable *records, const char *path)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (records);
  g_autofree char *text = NULL, *dir = g_path_get_dirname (path);
  g_ptr_array_sort (keys, cmp_str);
  json_builder_begin_object (b);
  for (guint i = 0; i < keys->len; i++)
    {
      BroCheckRecord *r = g_hash_table_lookup (records, keys->pdata[i]);
      json_builder_set_member_name (b, keys->pdata[i]);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "checkedAt");
      json_builder_add_int_value (b, r->checked_at);
      json_builder_set_member_name (b, "ok");
      json_builder_add_boolean_value (b, r->ok);
      json_builder_set_member_name (b, "summary");
      json_builder_add_string_value (b, r->summary ? r->summary : "");
      json_builder_end_object (b);
    }
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  text = bro_json_to_string (root, TRUE);
  g_mkdir_with_parents (dir, 0700);
  return bro_write_file_atomic (path, text, NULL);
}

void
bro_check_records_add (GHashTable *records, const BroFolderCheck *r, gint64 when)
{
  BroCheckRecord *rec = g_new0 (BroCheckRecord, 1);
  rec->checked_at = when;
  rec->ok = bro_folder_check_ok (r);
  rec->summary = bro_folder_check_summary (r);
  g_hash_table_replace (records, g_strdup (r->folder), rec);
}
