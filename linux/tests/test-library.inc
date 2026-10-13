/* test-library.inc: shared/fixtures/engine/library-service.cases.json, on a real executor, scheduler and problem
 * service, with a file system and database in memory (included by test-engine.c after test-services.inc). */
#include "bro-conflict-namer.h"
#include "bro-library-marker.h"
#include "bro-library-service.h"
#include "bro-sha256-hasher.h"

#define LIBRARY_FREE G_GINT64_CONSTANT (5000000000)

/* The case running, so a hang names it. */
static const char *library_case_id = "";

static char *
tree_parent (const char *p)
{
  const char *slash = strrchr (p, '/');
  return slash == p ? g_strdup ("/") : g_strndup (p, (gsize) (slash - p));
}

static gboolean
tree_error (BroBroError **error, BroMessageCode code, const char *path)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  if (error)
    *error = bro_bro_error_new (bro_message_code_wire (code), params, NULL);
  else
    bro_json_value_unref (params);
  return FALSE;
}

static gboolean
tree_failed (BroBroError **error, const char *operation, const char *path, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string (operation));
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  if (error)
    *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_FS_FAILED), params, NULL);
  else
    bro_json_value_unref (params);
  return FALSE;
}

/* ---- files and folders in memory, with the platform adapters' rules; calls can be made to fail or wait ---- */

typedef struct {
  char *op;
  char *path;
  BroJsonValue *error;
  int times; /* -1: every time */
} TreeFail;

static void
tree_fail_free (gpointer data)
{
  TreeFail *f = data;
  g_free (f->op);
  g_free (f->path);
  bro_json_value_unref (f->error);
  g_free (f);
}

#define TEST_TYPE_TREE (test_tree_get_type ())
G_DECLARE_FINAL_TYPE (TestTree, test_tree, TEST, TREE, GObject)

struct _TestTree {
  GObject parent;
  GMutex lock;
  GCond cond;
  GHashTable *files;  /* path → GBytes */
  GHashTable *dirs;   /* path → itself */
  GHashTable *modes;  /* path → GINT_TO_POINTER (mode) */
  GHashTable *synced; /* path → itself */
  GPtrArray *fails;   /* TreeFail * */
  GPtrArray *holds;   /* "op path" */
  gboolean released;
  int held;
};

static void tree_fs_iface (BroFileSystemInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestTree, test_tree, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, tree_fs_iface))

static void
test_tree_finalize (GObject *object)
{
  TestTree *t = TEST_TREE (object);
  g_hash_table_unref (t->files);
  g_hash_table_unref (t->dirs);
  g_hash_table_unref (t->modes);
  g_hash_table_unref (t->synced);
  g_ptr_array_unref (t->fails);
  g_ptr_array_unref (t->holds);
  g_mutex_clear (&t->lock);
  g_cond_clear (&t->cond);
  G_OBJECT_CLASS (test_tree_parent_class)->finalize (object);
}

static void
test_tree_class_init (TestTreeClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_tree_finalize;
}

static void
test_tree_init (TestTree *t)
{
  g_mutex_init (&t->lock);
  g_cond_init (&t->cond);
  t->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
  t->dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  t->modes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  t->synced = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  t->fails = g_ptr_array_new_with_free_func (tree_fail_free);
  t->holds = g_ptr_array_new_with_free_func (g_free);
  g_hash_table_add (t->dirs, g_strdup ("/"));
}

static int
tree_held (TestTree *t)
{
  int n;
  g_mutex_lock (&t->lock);
  n = t->held;
  g_mutex_unlock (&t->lock);
  return n;
}

static void
tree_release (TestTree *t)
{
  g_mutex_lock (&t->lock);
  t->released = TRUE;
  g_cond_broadcast (&t->cond);
  g_mutex_unlock (&t->lock);
}

static gboolean
tree_before (TestTree *t, const char *op, const char *path, BroBroError **error)
{
  g_autofree char *hold = g_strconcat (op, " ", path, NULL);
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  gboolean wait = FALSE;
  g_mutex_lock (&t->lock);
  for (guint i = 0; i < t->fails->len; i++)
    {
      TreeFail *f = t->fails->pdata[i];
      if (strcmp (f->op, op) != 0 || strcmp (f->path, path) != 0 || f->times == 0)
        continue;
      if (f->times > 0)
        f->times--;
      if (error)
        *error = bro_bro_error_new (bro_json_value_get_string (bro_json_value_member (f->error, "code"), ""),
                                    bro_json_value_ref (bro_json_value_member (f->error, "params")), NULL);
      g_mutex_unlock (&t->lock);
      return FALSE;
    }
  for (guint i = 0; i < t->holds->len && !t->released; i++)
    wait = wait || strcmp (t->holds->pdata[i], hold) == 0;
  if (wait)
    {
      t->held++;
      while (!t->released)
        if (!g_cond_wait_until (&t->cond, &t->lock, until))
          g_error ("%s: a held call was never released", library_case_id);
      t->held--;
    }
  g_mutex_unlock (&t->lock);
  return TRUE;
}

/* Under the lock. */
static void
tree_make_dirs_locked (TestTree *t, const char *dir)
{
  g_autofree char *d = g_strdup (dir);
  while (strcmp (d, "/") != 0 && !g_hash_table_contains (t->dirs, d))
    {
      char *up = tree_parent (d);
      g_hash_table_add (t->dirs, g_strdup (d));
      g_free (d);
      d = up;
    }
}

static void
tree_make_dirs (TestTree *t, const char *dir)
{
  g_mutex_lock (&t->lock);
  tree_make_dirs_locked (t, dir);
  g_mutex_unlock (&t->lock);
}

static void
tree_put (TestTree *t, const char *path, const char *text)
{
  g_autofree char *parent = tree_parent (path);
  g_mutex_lock (&t->lock);
  tree_make_dirs_locked (t, parent);
  g_hash_table_replace (t->files, g_strdup (path), g_bytes_new (text, strlen (text)));
  g_mutex_unlock (&t->lock);
}

static gboolean
under (const char *p, const char *root)
{
  gsize n = strlen (root);
  return strcmp (p, root) == 0 || (strncmp (p, root, n) == 0 && p[n] == '/');
}

static void
tree_move (TestTree *t, const char *from, const char *to)
{
  g_autoptr (GPtrArray) files = g_ptr_array_new ();
  g_autoptr (GPtrArray) dirs = g_ptr_array_new ();
  GHashTableIter it;
  gpointer key;
  gsize n = strlen (from);
  g_mutex_lock (&t->lock);
  g_hash_table_iter_init (&it, t->files);
  while (g_hash_table_iter_next (&it, &key, NULL))
    if (under (key, from))
      g_ptr_array_add (files, g_strdup (key));
  g_hash_table_iter_init (&it, t->dirs);
  while (g_hash_table_iter_next (&it, &key, NULL))
    if (under (key, from))
      g_ptr_array_add (dirs, g_strdup (key));
  for (guint i = 0; i < files->len; i++)
    {
      char *f = files->pdata[i];
      gpointer old_key;
      gpointer bytes;
      gpointer mode;
      char *dst = g_strconcat (to, f + n, NULL);
      g_hash_table_steal_extended (t->files, f, &old_key, &bytes);
      g_free (old_key);
      g_hash_table_replace (t->files, g_strdup (dst), bytes);
      if (g_hash_table_lookup_extended (t->modes, f, NULL, &mode))
        {
          g_hash_table_remove (t->modes, f);
          g_hash_table_replace (t->modes, g_strdup (dst), mode);
        }
      g_free (dst);
      g_free (f);
    }
  for (guint i = 0; i < dirs->len; i++)
    {
      char *d = dirs->pdata[i];
      g_hash_table_remove (t->dirs, d);
      g_hash_table_add (t->dirs, g_strconcat (to, d + n, NULL));
      g_free (d);
    }
  g_mutex_unlock (&t->lock);
}

static gboolean
tree_has (TestTree *t, const char *path, gboolean *is_dir)
{
  gboolean file, dir;
  g_mutex_lock (&t->lock);
  file = g_hash_table_contains (t->files, path);
  dir = g_hash_table_contains (t->dirs, path);
  g_mutex_unlock (&t->lock);
  if (is_dir)
    *is_dir = dir;
  return file || dir;
}

static gboolean
tree_exists (BroFileSystem *fs, const char *path)
{
  return tree_has (TEST_TREE (fs), path, NULL);
}

static BroFileInfo *
tree_stat (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  BroFileInfo *info = NULL;
  GBytes *b;
  if (!tree_before (t, "stat", path, error))
    return NULL;
  g_mutex_lock (&t->lock);
  b = g_hash_table_lookup (t->files, path);
  if (b || g_hash_table_contains (t->dirs, path))
    {
      info = bro_file_info_new ();
      info->size = b ? (gint64) g_bytes_get_size (b) : 0;
      info->is_directory = b == NULL;
    }
  g_mutex_unlock (&t->lock);
  if (!info)
    tree_error (error, BRO_MSG_FS_NOT_FOUND, path);
  return info;
}

static GPtrArray *
tree_list (BroFileSystem *fs, const char *directory, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  GPtrArray *list;
  GHashTableIter it;
  gpointer key;
  if (!tree_before (t, "list", directory, error))
    return NULL;
  g_mutex_lock (&t->lock);
  if (!g_hash_table_contains (t->dirs, directory))
    {
      g_mutex_unlock (&t->lock);
      tree_error (error, BRO_MSG_FS_NOT_FOUND, directory);
      return NULL;
    }
  list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_directory_entry_free);
  for (int pass = 0; pass < 2; pass++)
    {
      g_hash_table_iter_init (&it, pass == 0 ? t->dirs : t->files);
      while (g_hash_table_iter_next (&it, &key, NULL))
        {
          g_autofree char *parent = NULL;
          BroDirectoryEntry *e;
          if (strcmp (key, directory) == 0)
            continue;
          parent = tree_parent (key);
          if (strcmp (parent, directory) != 0)
            continue;
          e = bro_directory_entry_new ();
          e->name = g_strdup (strrchr (key, '/') + 1);
          e->is_directory = pass == 0;
          g_ptr_array_add (list, e);
        }
    }
  g_mutex_unlock (&t->lock);
  return list;
}

static GBytes *
tree_read (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  GBytes *b;
  if (!tree_before (t, "read", path, error))
    return NULL;
  g_mutex_lock (&t->lock);
  b = g_hash_table_lookup (t->files, path);
  if (b)
    g_bytes_ref (b);
  g_mutex_unlock (&t->lock);
  if (!b)
    tree_error (error, BRO_MSG_FS_NOT_FOUND, path);
  return b;
}

static gboolean
tree_create_directory (BroFileSystem *fs, const char *path, gboolean parents_must_exist, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  g_autofree char *parent = tree_parent (path);
  gboolean is_dir;
  if (!tree_before (t, "createDirectory", path, error))
    return FALSE;
  if (tree_has (t, path, &is_dir))
    return is_dir ? TRUE : tree_error (error, BRO_MSG_FS_ALREADY_EXISTS, path);
  if (parents_must_exist && !(tree_has (t, parent, &is_dir) && is_dir))
    return tree_error (error, BRO_MSG_FS_PARENT_MISSING, parent);
  tree_make_dirs (t, path);
  return TRUE;
}

static gboolean
tree_write_atomically (BroFileSystem *fs, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  g_autofree char *parent = tree_parent (path);
  gboolean is_dir;
  if (!tree_before (t, "writeAtomically", path, error))
    return FALSE;
  if (!(tree_has (t, parent, &is_dir) && is_dir))
    return tree_error (error, BRO_MSG_FS_PARENT_MISSING, parent);
  g_mutex_lock (&t->lock);
  g_hash_table_replace (t->files, g_strdup (path), g_bytes_ref (bytes));
  g_hash_table_replace (t->modes, g_strdup (path), GINT_TO_POINTER (mode));
  g_mutex_unlock (&t->lock);
  return TRUE;
}

static gboolean
tree_rename (BroFileSystem *fs, const char *from, const char *to, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  g_autofree char *parent = tree_parent (to);
  gboolean is_dir;
  if (!tree_before (t, "rename", from, error))
    return FALSE;
  if (!tree_has (t, from, NULL))
    return tree_error (error, BRO_MSG_FS_NOT_FOUND, from);
  if (tree_has (t, to, NULL))
    return tree_error (error, BRO_MSG_FS_ALREADY_EXISTS, to);
  if (!(tree_has (t, parent, &is_dir) && is_dir))
    return tree_error (error, BRO_MSG_FS_PARENT_MISSING, parent);
  tree_move (t, from, to);
  return TRUE;
}

static gboolean tree_remove (BroFileSystem *fs, const char *path, BroBroError **error);

static gint
tree_by_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

static gboolean
tree_merge (TestTree *t, const char *source, const char *target, GPtrArray *moved, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = tree_list (BRO_FILE_SYSTEM (t), source, error);
  if (!entries)
    return FALSE;
  g_ptr_array_sort (entries, tree_by_name);
  for (guint i = 0; i < entries->len; i++)
    {
      BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *src = g_strconcat (source, "/", e->name, NULL);
      g_autoptr (GPtrArray) existing = tree_list (BRO_FILE_SYSTEM (t), target, error);
      g_autoptr (GPtrArray) names = NULL;
      g_autofree char *lower = g_ascii_strdown (e->name, -1);
      g_autofree char *chosen = NULL;
      g_autofree char *dst = NULL;
      BroDirectoryEntry *same = NULL;
      if (!existing)
        return FALSE;
      names = g_ptr_array_new ();
      for (guint j = 0; j < existing->len; j++)
        {
          BroDirectoryEntry *x = existing->pdata[j];
          g_autofree char *x_lower = g_ascii_strdown (x->name, -1);
          g_ptr_array_add (names, x->name);
          if (x->is_directory && strcmp (x_lower, lower) == 0)
            same = x;
        }
      g_ptr_array_add (names, NULL);
      if (e->is_directory && same)
        {
          g_autofree char *into = g_strconcat (target, "/", same->name, NULL);
          if (!tree_merge (t, src, into, moved, error) || !tree_remove (BRO_FILE_SYSTEM (t), src, error))
            return FALSE;
          continue;
        }
      chosen = bro_conflict_namer_next (e->name, (const char *const *) names->pdata, e->is_directory);
      dst = g_strconcat (target, "/", chosen, NULL);
      if (!tree_rename (BRO_FILE_SYSTEM (t), src, dst, error))
        return FALSE;
      if (!e->is_directory)
        {
          BroMovedItem *m = bro_moved_item_new ();
          m->from = g_strdup (src);
          m->to = g_strdup (dst);
          g_ptr_array_add (moved, m);
        }
    }
  return TRUE;
}

static GPtrArray *
tree_move_merging (BroFileSystem *fs, const char *from, const char *to, BroMovePolicy policy, BroMovedFunc on_moved, gpointer data,
                   BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  GPtrArray *moved;
  gboolean is_dir;
  if (!tree_before (t, "moveMerging", from, error))
    return NULL;
  if (!tree_has (t, from, &is_dir))
    {
      tree_error (error, BRO_MSG_FS_NOT_FOUND, from);
      return NULL;
    }
  if (!is_dir)
    {
      tree_failed (error, "move", from, "it isn't a folder");
      return NULL;
    }
  if (!tree_has (t, to, &is_dir))
    {
      if (!tree_create_directory (fs, to, TRUE, error))
        return NULL;
    }
  else if (!is_dir)
    {
      tree_error (error, BRO_MSG_FS_ALREADY_EXISTS, to);
      return NULL;
    }
  moved = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_moved_item_free);
  if (!tree_merge (t, from, to, moved, error))
    {
      g_ptr_array_unref (moved);
      return NULL;
    }
  return moved;
}

static gboolean
tree_remove (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  GHashTableIter it;
  gpointer key;
  gboolean busy = FALSE;
  if (!tree_before (t, "remove", path, error))
    return FALSE;
  g_mutex_lock (&t->lock);
  if (g_hash_table_remove (t->files, path))
    {
      g_mutex_unlock (&t->lock);
      return TRUE;
    }
  if (!g_hash_table_contains (t->dirs, path))
    {
      g_mutex_unlock (&t->lock);
      return tree_error (error, BRO_MSG_FS_NOT_FOUND, path);
    }
  for (int pass = 0; pass < 2 && !busy; pass++)
    {
      g_hash_table_iter_init (&it, pass == 0 ? t->files : t->dirs);
      while (g_hash_table_iter_next (&it, &key, NULL))
        busy = busy || (strcmp (key, path) != 0 && under (key, path));
    }
  if (!busy)
    g_hash_table_remove (t->dirs, path);
  g_mutex_unlock (&t->lock);
  return busy ? tree_failed (error, "remove", path, "the folder isn't empty") : TRUE;
}

static gboolean
tree_sync_directory (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestTree *t = TEST_TREE (fs);
  gboolean is_dir;
  if (!tree_before (t, "syncDirectory", path, error))
    return FALSE;
  if (!(tree_has (t, path, &is_dir) && is_dir))
    return tree_error (error, BRO_MSG_FS_NOT_FOUND, path);
  g_mutex_lock (&t->lock);
  g_hash_table_add (t->synced, g_strdup (path));
  g_mutex_unlock (&t->lock);
  return TRUE;
}

/* A file read as a stream, four bytes at a time. */
#define TEST_TYPE_TREE_STREAM (test_tree_stream_get_type ())
G_DECLARE_FINAL_TYPE (TestTreeStream, test_tree_stream, TEST, TREE_STREAM, GObject)

struct _TestTreeStream {
  GObject parent;
  GBytes *bytes;
  gsize at;
};

static GBytes *
stream_read (BroByteStream *s, int max_bytes, BroBroError **error)
{
  TestTreeStream *self = TEST_TREE_STREAM (s);
  gsize size;
  const guint8 *data = g_bytes_get_data (self->bytes, &size);
  gsize n = MIN (MIN ((gsize) max_bytes, 4), size - self->at);
  GBytes *chunk = g_bytes_new (data + self->at, n);
  self->at += n;
  return chunk;
}

static void
stream_close (BroByteStream *s)
{
}

static void
tree_stream_iface (BroByteStreamInterface *iface)
{
  iface->read = stream_read;
  iface->close = stream_close;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (TestTreeStream, test_tree_stream, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_BYTE_STREAM, tree_stream_iface))

static void
test_tree_stream_finalize (GObject *object)
{
  g_bytes_unref (TEST_TREE_STREAM (object)->bytes);
  G_OBJECT_CLASS (test_tree_stream_parent_class)->finalize (object);
}

static void
test_tree_stream_class_init (TestTreeStreamClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_tree_stream_finalize;
}

static void
test_tree_stream_init (TestTreeStream *self)
{
}

static BroByteStream *
tree_open_for_reading (BroFileSystem *fs, const char *path, gboolean bypass_cache, BroBroError **error)
{
  GBytes *bytes = tree_read (fs, path, error);
  TestTreeStream *s;
  if (!bytes)
    return NULL;
  s = g_object_new (TEST_TYPE_TREE_STREAM, NULL);
  s->bytes = bytes;
  return BRO_BYTE_STREAM (s);
}

static BroVolumeInfo *
tree_volume (BroFileSystem *fs, const char *path, BroBroError **error)
{
  BroVolumeInfo *v;
  if (!tree_before (TEST_TREE (fs), "volume", path, error))
    return NULL;
  v = bro_volume_info_new ();
  v->id = g_strdup ("volume");
  v->free_bytes = LIBRARY_FREE;
  v->fs_type = g_strdup ("apfs");
  v->case_sensitive = TRUE;
  return v;
}

static void
tree_fs_iface (BroFileSystemInterface *iface)
{
  iface->exists = tree_exists;
  iface->stat = tree_stat;
  iface->list = tree_list;
  iface->read = tree_read;
  iface->create_directory = tree_create_directory;
  iface->write_atomically = tree_write_atomically;
  iface->rename = tree_rename;
  iface->move_merging = tree_move_merging;
  iface->remove = tree_remove;
  iface->sync_directory = tree_sync_directory;
  iface->open_for_reading = tree_open_for_reading;
  iface->volume = tree_volume;
}

/* ---- the libraries table and the unit tables in memory; a unit's library must be stored (the foreign key) ---- */

#define TEST_TYPE_LIBRARY_DB (test_library_db_get_type ())
G_DECLARE_FINAL_TYPE (TestLibraryDb, test_library_db, TEST, LIBRARY_DB, GObject)

struct _TestLibraryDb {
  GObject parent;
  GMutex lock;
  GHashTable *libraries; /* id → BroLibraryRecord */
  GHashTable *units;     /* id → BroUnitRecord */
  GHashTable *intents;   /* unit id → BroCommitIntent */
  GHashTable *files;     /* unit id → GPtrArray of BroUnitFile */
};

static void db_libraries_iface (BroLibraryRepositoryInterface *iface);
static void db_units_iface (BroUnitRepositoryInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestLibraryDb, test_library_db, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_LIBRARY_REPOSITORY, db_libraries_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_UNIT_REPOSITORY, db_units_iface))

static void
test_library_db_finalize (GObject *object)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (object);
  g_hash_table_unref (db->libraries);
  g_hash_table_unref (db->units);
  g_hash_table_unref (db->intents);
  g_hash_table_unref (db->files);
  g_mutex_clear (&db->lock);
  G_OBJECT_CLASS (test_library_db_parent_class)->finalize (object);
}

static void
test_library_db_class_init (TestLibraryDbClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_library_db_finalize;
}

static void
test_library_db_init (TestLibraryDb *db)
{
  g_mutex_init (&db->lock);
  db->libraries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_library_record_free);
  db->units = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_unit_record_free);
  db->intents = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_commit_intent_free);
  db->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
}

static gint
library_by_id (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroLibraryRecord *const *) a)->id, (*(BroLibraryRecord *const *) b)->id);
}

static GPtrArray *
db_all (BroLibraryRepository *repo, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  GPtrArray *all = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_library_record_free);
  GHashTableIter it;
  gpointer value;
  g_mutex_lock (&db->lock);
  g_hash_table_iter_init (&it, db->libraries);
  while (g_hash_table_iter_next (&it, NULL, &value))
    g_ptr_array_add (all, bro_library_record_copy (value));
  g_mutex_unlock (&db->lock);
  g_ptr_array_sort (all, library_by_id);
  return all;
}

static BroLibraryRecord *
db_get (BroLibraryRepository *repo, const char *library_id, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  BroLibraryRecord *r;
  g_mutex_lock (&db->lock);
  r = g_hash_table_lookup (db->libraries, library_id);
  r = r ? bro_library_record_copy (r) : NULL;
  g_mutex_unlock (&db->lock);
  return r;
}

static gboolean
db_upsert (BroLibraryRepository *repo, const BroLibraryRecord *library, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  g_mutex_lock (&db->lock);
  g_hash_table_replace (db->libraries, g_strdup (library->id), bro_library_record_copy (library));
  g_mutex_unlock (&db->lock);
  return TRUE;
}

static gboolean
db_set_seen (BroLibraryRepository *repo, const char *library_id, gboolean online, BroInstant at, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  BroLibraryRecord *r;
  g_mutex_lock (&db->lock);
  r = g_hash_table_lookup (db->libraries, library_id);
  if (r)
    {
      r->online = online;
      if (online)
        {
          r->has_last_seen_at = TRUE;
          r->last_seen_at = at;
        }
    }
  g_mutex_unlock (&db->lock);
  return TRUE;
}

static void
db_libraries_iface (BroLibraryRepositoryInterface *iface)
{
  iface->all = db_all;
  iface->get = db_get;
  iface->upsert = db_upsert;
  iface->set_seen = db_set_seen;
}

static gboolean
db_begin_commit (BroUnitRepository *repo, const BroUnitRecord *unit, const BroCommitIntent *intent, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  gboolean stored;
  g_mutex_lock (&db->lock);
  stored = g_hash_table_contains (db->libraries, unit->library_id);
  if (stored)
    {
      g_hash_table_replace (db->units, g_strdup (unit->id.value), bro_unit_record_copy (unit));
      g_hash_table_replace (db->intents, g_strdup (unit->id.value), bro_commit_intent_copy (intent));
    }
  g_mutex_unlock (&db->lock);
  if (!stored)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "reason", bro_json_value_new_string ("FOREIGN KEY"));
      *error = bro_bro_error_new ("store.failed", params, NULL);
    }
  return stored;
}

static gboolean
db_mark_moved (BroUnitRepository *repo, BroId unit_id, int seq, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  BroCommitIntent *intent;
  g_mutex_lock (&db->lock);
  intent = g_hash_table_lookup (db->intents, unit_id.value);
  for (guint i = 0; intent && i < intent->items->len; i++)
    if (((BroCommitItem *) intent->items->pdata[i])->seq == seq)
      ((BroCommitItem *) intent->items->pdata[i])->moved = TRUE;
  g_mutex_unlock (&db->lock);
  return TRUE;
}

static gboolean
db_finish_commit (BroUnitRepository *repo, const BroCommitIntent *intent, const BroUnitRecord *record, GPtrArray *files, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  GPtrArray *copy = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (copy, bro_unit_file_copy (files->pdata[i]));
  g_mutex_lock (&db->lock);
  g_hash_table_replace (db->units, g_strdup (record->id.value), bro_unit_record_copy (record));
  g_hash_table_replace (db->files, g_strdup (record->id.value), copy);
  g_hash_table_remove (db->intents, intent->unit_id.value);
  g_mutex_unlock (&db->lock);
  return TRUE;
}

static gint
intent_by_age (gconstpointer a, gconstpointer b)
{
  const BroCommitIntent *x = *(BroCommitIntent *const *) a;
  const BroCommitIntent *y = *(BroCommitIntent *const *) b;
  if (x->created_at.unix_milliseconds != y->created_at.unix_milliseconds)
    return x->created_at.unix_milliseconds < y->created_at.unix_milliseconds ? -1 : 1;
  return strcmp (x->unit_id.value, y->unit_id.value);
}

static GPtrArray *
db_open_intents (BroUnitRepository *repo, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  GPtrArray *all = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_commit_intent_free);
  GHashTableIter it;
  gpointer value;
  g_mutex_lock (&db->lock);
  g_hash_table_iter_init (&it, db->intents);
  while (g_hash_table_iter_next (&it, NULL, &value))
    g_ptr_array_add (all, bro_commit_intent_copy (value));
  g_mutex_unlock (&db->lock);
  g_ptr_array_sort (all, intent_by_age);
  return all;
}

static BroUnitRecord *
db_unit_get (BroUnitRepository *repo, BroId unit_id, BroBroError **error)
{
  TestLibraryDb *db = TEST_LIBRARY_DB (repo);
  BroUnitRecord *u;
  g_mutex_lock (&db->lock);
  u = g_hash_table_lookup (db->units, unit_id.value);
  u = u ? bro_unit_record_copy (u) : NULL;
  g_mutex_unlock (&db->lock);
  return u;
}

static void
db_units_iface (BroUnitRepositoryInterface *iface)
{
  iface->begin_commit = db_begin_commit;
  iface->mark_moved = db_mark_moved;
  iface->finish_commit = db_finish_commit;
  iface->open_intents = db_open_intents;
  iface->get = db_unit_get;
}

/* ---- the harness ---- */

typedef struct {
  BroEngineExecutor *executor;
  TestManualClock *clock;
  BroEngineClock *engine_clock;
  TestTree *tree;
  TestLibraryDb *db;
  TestCaseConfig *config;
  BroEventBus *bus;
  BroProblemService *problems;
  BroScheduler *scheduler;
  BroLibraryService *service;
  BroSubscription *subscription;
  GPtrArray *events; /* BroEngineEvent */
  GMutex lock;
  GCond cond;
  BroJsonValue **results;
  gboolean *done;
  guint n_ops;
  const char *crash_at;
  gboolean crashed;
} LibraryHarness;

static BroJsonValue *
library_error_json (const BroBroError *error)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *e;
  if (strcmp (error->code, "crash") == 0)
    {
      bro_json_value_set (o, "crashed", bro_json_value_ref (bro_json_value_member (error->params, "point")));
      return o;
    }
  e = bro_json_value_new_object ();
  bro_json_value_set (e, "code", bro_json_value_new_string (error->code));
  if (bro_json_value_length (error->params) > 0)
    bro_json_value_set (e, "params", bro_json_value_ref (error->params));
  bro_json_value_set (o, "error", e);
  return o;
}

static void
library_record (LibraryHarness *h, guint index, BroJsonValue *result)
{
  g_mutex_lock (&h->lock);
  if (h->results[index])
    bro_json_value_unref (h->results[index]);
  h->results[index] = result;
  h->done[index] = TRUE;
  g_cond_broadcast (&h->cond);
  g_mutex_unlock (&h->lock);
}

static void
library_wait_for (LibraryHarness *h, guint index)
{
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&h->lock);
  while (!h->done[index])
    if (!g_cond_wait_until (&h->cond, &h->lock, until))
      g_error ("%s: op %u didn't finish", library_case_id, index);
  g_mutex_unlock (&h->lock);
}

typedef struct {
  LibraryHarness *h;
  gboolean ok;
  gboolean checked;
} SettleCheck;

static void
settle_check_action (gpointer data)
{
  SettleCheck *c = data;
  /* Posted alone (no barrier after it), so the executor's activity counts only the engine's own work. */
  gboolean ok = bro_engine_executor_activity (c->h->executor) == tree_held (c->h->tree);
  g_mutex_lock (&c->h->lock);
  c->ok = ok;
  c->checked = TRUE;
  g_cond_broadcast (&c->h->cond);
  g_mutex_unlock (&c->h->lock);
}

static void
library_settle (gpointer data)
{
  LibraryHarness *h = data;
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  if (bro_engine_executor_is_current (h->executor))
    return;
  for (;;)
    {
      SettleCheck c = { h, FALSE, FALSE };
      gboolean ok;
      if (!bro_engine_executor_post (h->executor, settle_check_action, &c, NULL))
        return;
      g_mutex_lock (&h->lock);
      while (!c.checked)
        g_cond_wait (&h->cond, &h->lock);
      ok = c.ok;
      g_mutex_unlock (&h->lock);
      if (ok)
        return;
      if (g_get_monotonic_time () > until)
        g_error ("%s: the service didn't settle", library_case_id);
      g_usleep (1000);
    }
}

static gboolean
crash_cb (const char *point, gpointer data, BroBroError **error)
{
  LibraryHarness *h = data;
  BroJsonValue *params;
  gboolean now;
  g_mutex_lock (&h->lock);
  now = h->crash_at && strcmp (point, h->crash_at) == 0 && !h->crashed;
  if (now)
    h->crashed = TRUE;
  g_mutex_unlock (&h->lock);
  if (!now)
    return TRUE;
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "point", bro_json_value_new_string (point));
  *error = bro_bro_error_new ("crash", params, NULL);
  return FALSE;
}

static void
library_event_seen (BroEngineEvent *e, gpointer data)
{
  g_ptr_array_add (data, bro_engine_event_ref (e));
}

static void
build_action (gpointer data)
{
  LibraryHarness *h = data;
  static const BroEngineEventType types[] = { BRO_ENGINE_EVENT_TYPE_LIBRARY_CHANGED, BRO_ENGINE_EVENT_TYPE_PROBLEM_RAISED,
                                              BRO_ENGINE_EVENT_TYPE_PROBLEM_CLEARED, BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED };
  g_autoptr (BroSchedulerLimits) limits = bro_scheduler_limits_new ();
  if (h->service)
    bro_library_service_stop (h->service);
  g_clear_object (&h->service);
  if (h->subscription)
    bro_subscription_cancel (h->subscription);
  g_clear_object (&h->subscription);
  g_clear_object (&h->scheduler);
  g_clear_object (&h->problems);
  g_clear_object (&h->bus);
  h->bus = bro_event_bus_new (BRO_CLOCK (h->clock), 1, 1024, 1024);
  h->problems = bro_problem_service_new (h->bus, BRO_CLOCK (h->clock));
  h->scheduler = bro_scheduler_new (h->executor, h->bus, limits);
  h->service = bro_library_service_new (h->executor, h->bus, h->problems, h->scheduler, BRO_CONFIG_SOURCE (h->config), h->engine_clock,
                                        BRO_FILE_SYSTEM (h->tree), BRO_LIBRARY_REPOSITORY (h->db), BRO_UNIT_REPOSITORY (h->db), crash_cb, h);
  h->subscription = bro_event_bus_subscribe (h->bus, types, G_N_ELEMENTS (types), library_event_seen, h->events, NULL);
}

typedef struct {
  LibraryHarness *h;
  guint index;
  BroJsonValue *arg;
  /* for the worker ops */
  BroUnitRecord *unit;
  BroCommitIntent *intent;
  GPtrArray *files;
  char *tag;
  BroJsonValue *result;
} LibraryOp;

static void
library_op_free (gpointer data)
{
  LibraryOp *o = data;
  bro_unit_record_free (o->unit);
  bro_commit_intent_free (o->intent);
  if (o->files)
    g_ptr_array_unref (o->files);
  g_free (o->tag);
  if (o->result)
    bro_json_value_unref (o->result);
  g_free (o);
}

static void
op_done (const BroBroError *error, gpointer data)
{
  LibraryOp *o = data;
  library_record (o->h, o->index, error ? library_error_json (error) : bro_json_value_new_string ("ok"));
}

static void
op_dto_done (BroLibraryDto *library, const BroBroError *error, gpointer data)
{
  LibraryOp *o = data;
  library_record (o->h, o->index, error ? library_error_json (error) : bro_library_dto_json (library));
  bro_library_dto_free (library);
}

static void
start_action (gpointer data)
{
  LibraryOp *o = data;
  bro_library_service_start (o->h->service, op_done, o, library_op_free);
}

static void
check_op_action (gpointer data)
{
  LibraryOp *o = data;
  bro_library_service_check (o->h->service, op_done, o, library_op_free);
}

static void
attach_action (gpointer data)
{
  LibraryOp *o = data;
  bro_library_service_attach (o->h->service, bro_json_value_get_string (bro_json_value_member (o->arg, "id"), ""),
                              bro_json_value_get_string (bro_json_value_member (o->arg, "path"), ""),
                              bro_json_value_get_string (bro_json_value_member (o->arg, "name"), ""), op_dto_done, o, library_op_free);
}

static BroJsonValue *
result_json (const BroCommitResult *r)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *files = bro_json_value_new_array ();
  g_autoptr (GPtrArray) paths = g_ptr_array_new ();
  for (guint i = 0; i < r->files->len; i++)
    g_ptr_array_add (paths, ((BroUnitFile *) r->files->pdata[i])->path);
  g_ptr_array_sort (paths, compare_strings_in_array);
  for (guint i = 0; i < paths->len; i++)
    bro_json_value_append (files, bro_json_value_new_string (paths->pdata[i]));
  bro_json_value_set (o, "unitId", bro_json_value_new_string (r->unit_id.value));
  bro_json_value_set (o, "destination", bro_json_value_new_string (r->destination));
  bro_json_value_set (o, "files", files);
  bro_json_value_set (o, "merged", bro_json_value_new_bool (r->merged));
  return o;
}

/* A step's worker calling the port: counted in the executor's activity, as it would be. */
static gboolean
commit_work (gpointer data, BroBroError **unused)
{
  LibraryOp *o = data;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroCommitResult) r = NULL;
  if (o->tag)
    r = bro_library_port_quarantine (BRO_LIBRARY_PORT (o->h->service), o->unit, o->intent, o->files, o->tag, &error);
  else
    r = bro_library_port_commit (BRO_LIBRARY_PORT (o->h->service), o->unit, o->intent, o->files, &error);
  o->result = r ? result_json (r) : library_error_json (error);
  return TRUE;
}

static gboolean
roll_forward_work (gpointer data, BroBroError **unused)
{
  LibraryOp *o = data;
  g_autoptr (GPtrArray) open = bro_unit_repository_open_intents (BRO_UNIT_REPOSITORY (o->h->db), NULL);
  o->result = bro_json_value_new_array ();
  for (guint i = 0; i < open->len; i++)
    {
      g_autoptr (BroBroError) error = NULL;
      g_autoptr (BroCommitResult) r = bro_library_service_roll_forward (o->h->service, open->pdata[i], &error);
      bro_json_value_append (o->result, r ? result_json (r) : library_error_json (error));
    }
  return TRUE;
}

static void
worker_op_done (gpointer data, const BroBroError *error)
{
  LibraryOp *o = data;
  library_record (o->h, o->index, g_steal_pointer (&o->result));
}

static void
worker_op_action (gpointer data)
{
  LibraryOp *o = data;
  bro_engine_executor_worker (o->h->executor, o->unit ? commit_work : roll_forward_work, worker_op_done, o, library_op_free);
}

static BroJsonValue *
intents_json (TestLibraryDb *db)
{
  g_autoptr (GPtrArray) open = db_open_intents (BRO_UNIT_REPOSITORY (db), NULL);
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < open->len; i++)
    {
      BroCommitIntent *intent = open->pdata[i];
      BroJsonValue *pair = bro_json_value_new_array ();
      BroJsonValue *items = bro_json_value_new_array ();
      for (guint j = 0; j < intent->items->len; j++)
        {
          BroCommitItem *x = intent->items->pdata[j];
          g_autofree char *line = g_strdup_printf ("%d %s %s%s%s", x->seq, x->from_path, x->to_path, x->is_dir ? " dir" : "", x->moved ? " moved" : "");
          bro_json_value_append (items, bro_json_value_new_string (line));
        }
      bro_json_value_append (pair, bro_json_value_new_string (intent->unit_id.value));
      bro_json_value_append (pair, items);
      bro_json_value_append (a, pair);
    }
  return a;
}

typedef struct {
  LibraryHarness *h;
  BroJsonValue *arg;
  BroId probe;
  char *code;
  BroLease *lease;
  BroCancellationSource *cancel;
} BlockedProbe;

static void
probe_wait (const BroWait *wait, gpointer data)
{
  BlockedProbe *p = data;
  if (wait->kind == BRO_WAIT_BLOCKED)
    {
      g_free (p->code);
      p->code = g_strdup (wait->reason->code);
    }
}

static void
probe_done (BroLease *lease, const BroBroError *error, gpointer data)
{
  BlockedProbe *p = data;
  if (lease)
    p->lease = lease;
}

static void
probe_start_action (gpointer data)
{
  BlockedProbe *p = data;
  g_autoptr (BroResourceSet) resources = bro_resource_set_new ();
  const char *writes[] = { bro_json_value_get_string (p->arg, ""), NULL };
  resources->library_writes = g_strdupv ((char **) writes);
  bro_scheduler_submit (p->h->scheduler, &p->probe, BRO_QUEUE_PROCESSING);
  bro_scheduler_acquire (p->h->scheduler, &p->probe, resources, probe_wait, bro_cancellation_source_token (p->cancel), probe_done, p, NULL, NULL);
}

static void
probe_end_action (gpointer data)
{
  BlockedProbe *p = data;
  if (p->lease)
    bro_scheduler_release (p->h->scheduler, p->lease);
  bro_scheduler_finish (p->h->scheduler, &p->probe);
}

static void
libraries_action (gpointer data)
{
  LibraryOp *o = data;
  g_autoptr (GPtrArray) list = bro_library_service_libraries (o->h->service);
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < list->len; i++)
    bro_json_value_append (a, bro_library_dto_json (list->pdata[i]));
  library_record (o->h, o->index, a);
}

static void
configure_action (gpointer data)
{
  LibraryOp *o = data;
  g_autoptr (BroJsonValue) version = bro_json_value_new_object ();
  bro_json_value_unref (o->h->config->document);
  o->h->config->document = bro_json_value_new_object ();
  bro_json_value_set (o->h->config->document, "version", bro_json_value_new_integer (3));
  bro_json_value_set (o->h->config->document, "libraries", bro_json_value_ref (o->arg));
  bro_json_value_set (version, "version", bro_json_value_new_integer (1));
  bro_engine_event_unref (bro_event_bus_publish (o->h->bus, BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED, version));
}

static void
stop_action (gpointer data)
{
  LibraryHarness *h = data;
  bro_library_service_stop (h->service);
}

static void
run_library_op (LibraryHarness *h, guint i, BroJsonValue *op, GPtrArray *async)
{
  const char *name = op->kind == BRO_JSON_VALUE_STRING ? op->string : op->keys->pdata[0];
  BroJsonValue *arg = op->kind == BRO_JSON_VALUE_OBJECT ? op->items->pdata[0] : NULL;
  gboolean wait = !arg || bro_json_value_get_bool (bro_json_value_member (arg, "wait"), TRUE);
  LibraryOp *o = g_new0 (LibraryOp, 1);
  o->h = h;
  o->index = i;
  o->arg = arg;
  if (strcmp (name, "start") == 0 || strcmp (name, "check") == 0 || strcmp (name, "attach") == 0)
    {
      bro_engine_executor_post (h->executor, strcmp (name, "start") == 0 ? start_action : strcmp (name, "check") == 0 ? check_op_action : attach_action, o, NULL);
      if (wait)
        library_wait_for (h, i);
      else
        g_ptr_array_add (async, GUINT_TO_POINTER (i));
      library_settle (h);
      return;
    }
  if (strcmp (name, "commit") == 0 || strcmp (name, "quarantine") == 0 || strcmp (name, "rollForward") == 0)
    {
      if (strcmp (name, "rollForward") != 0)
        {
          BroJsonValue *u = bro_json_value_member (arg, "unit");
          BroJsonValue *it = bro_json_value_member (arg, "intent");
          BroJsonValue *rows = bro_json_value_member (arg, "files");
          BroJsonValue *items = bro_json_value_member (it, "items");
          o->unit = bro_unit_record_new ();
          g_strlcpy (o->unit->id.value, bro_json_value_get_string (bro_json_value_member (u, "id"), ""), sizeof o->unit->id.value);
          o->unit->library_id = g_strdup ("lib");
          o->unit->has_job_id = TRUE;
          g_strlcpy (o->unit->job_id.value, "00000000-0000-4000-8000-000000000001", sizeof o->unit->job_id.value);
          o->unit->path = g_strdup (bro_json_value_get_string (bro_json_value_member (u, "path"), ""));
          o->unit->record_file = g_strdup ("bromelia-00000000.json");
          o->unit->record_version = 3;
          o->unit->state = BRO_UNIT_STATE_COMMITTING;
          o->unit->status = BRO_UNIT_STATUS_SUCCESS;
          o->unit->name = g_strdup (bro_json_value_get_string (bro_json_value_member (u, "name"), ""));
          o->unit->kind = BRO_MEDIA_KIND_MOVIE;
          o->unit->format = g_strdup ("bluray");
          o->unit->format_code = g_strdup ("BD");
          o->unit->label = g_strdup ("");
          o->unit->makemkv_version = g_strdup ("");
          o->unit->file_count = (int) bro_json_value_length (rows);
          o->unit->attempts = 1;
          o->unit->created_at = bro_clock_now (BRO_CLOCK (h->clock));
          o->intent = bro_commit_intent_new ();
          o->intent->unit_id = o->unit->id;
          o->intent->job_id = o->unit->job_id;
          o->intent->staging = g_strdup (bro_json_value_get_string (bro_json_value_member (it, "staging"), ""));
          o->intent->destination = g_strdup (bro_json_value_get_string (bro_json_value_member (it, "destination"), ""));
          o->intent->merge = bro_json_value_get_bool (bro_json_value_member (it, "merge"), FALSE);
          o->intent->created_at = o->unit->created_at;
          for (guint j = 0; j < bro_json_value_length (items); j++)
            {
              BroJsonValue *x = bro_json_value_at (items, j);
              BroCommitItem *item = bro_commit_item_new ();
              item->seq = (int) j;
              item->from_path = g_strdup (bro_json_value_get_string (bro_json_value_at (x, 0), ""));
              item->to_path = g_strdup (bro_json_value_get_string (bro_json_value_at (x, 1), ""));
              item->sha256 = g_strdup (bro_json_value_get_string (bro_json_value_at (x, 2), NULL));
              item->is_dir = bro_json_value_get_bool (bro_json_value_at (x, 3), FALSE);
              g_ptr_array_add (o->intent->items, item);
            }
          o->files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
          for (guint j = 0; j < bro_json_value_length (rows); j++)
            {
              BroJsonValue *f = bro_json_value_at (rows, j);
              BroUnitFile *file = bro_unit_file_new ();
              file->path = g_strdup (bro_json_value_get_string (bro_json_value_at (f, 0), ""));
              file->size = bro_json_value_get_integer (bro_json_value_at (f, 1), 0);
              file->sha256 = g_strdup (bro_json_value_get_string (bro_json_value_at (f, 2), ""));
              file->role = g_strdup (bro_json_value_get_string (bro_json_value_at (f, 3), ""));
              g_ptr_array_add (o->files, file);
            }
          if (strcmp (name, "quarantine") == 0)
            o->tag = g_strdup (bro_json_value_get_string (bro_json_value_member (arg, "tag"), ""));
        }
      bro_engine_executor_post (h->executor, worker_op_action, o, NULL);
      if (wait)
        library_wait_for (h, i);
      else
        {
          gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
          g_ptr_array_add (async, GUINT_TO_POINTER (i));
          while (tree_held (h->tree) == 0)
            {
              if (g_get_monotonic_time () > until)
                g_error ("%s: the op never reached its held call", library_case_id);
              g_usleep (1000);
            }
        }
      library_settle (h);
      return;
    }
  if (strcmp (name, "libraries") == 0)
    {
      bro_engine_executor_post (h->executor, libraries_action, o, g_free);
      library_wait_for (h, i);
      return;
    }
  if (strcmp (name, "require") == 0 || strcmp (name, "staging") == 0)
    {
      g_autoptr (BroBroError) error = NULL;
      if (strcmp (name, "require") == 0)
        {
          g_autoptr (BroLibraryHandle) handle = bro_library_port_require (BRO_LIBRARY_PORT (h->service), bro_json_value_get_string (arg, ""), &error);
          BroJsonValue *r = NULL;
          if (handle)
            {
              r = bro_json_value_new_object ();
              bro_json_value_set (r, "id", bro_json_value_new_string (handle->id));
              bro_json_value_set (r, "path", bro_json_value_new_string (handle->path));
            }
          library_record (h, i, r ? r : library_error_json (error));
        }
      else
        {
          BroId job;
          g_autofree char *dir = NULL;
          g_strlcpy (job.value, bro_json_value_get_string (bro_json_value_member (arg, "job"), ""), sizeof job.value);
          dir = bro_library_port_staging_directory (BRO_LIBRARY_PORT (h->service), bro_json_value_get_string (bro_json_value_member (arg, "library"), ""),
                                                    &job, &error);
          library_record (h, i, dir ? bro_json_value_new_string (dir) : library_error_json (error));
        }
      g_free (o);
      return;
    }
  if (strcmp (name, "blocked") == 0)
    {
      BlockedProbe p = { h, arg, { { 0 } }, NULL, NULL, bro_cancellation_source_new () };
      g_snprintf (p.probe.value, sizeof p.probe.value, "probe-%u", i);
      bro_engine_executor_post (h->executor, probe_start_action, &p, NULL);
      library_settle (h);
      bro_engine_executor_post (h->executor, probe_end_action, &p, NULL);
      library_settle (h);
      library_record (h, i, p.lease ? bro_json_value_new_null () : p.code ? bro_json_value_new_string (p.code) : bro_json_value_new_null ());
      g_free (p.code);
      g_clear_pointer (&p.lease, bro_lease_unref);
      bro_cancellation_source_free (p.cancel);
      g_free (o);
      return;
    }
  if (strcmp (name, "configure") == 0)
    {
      bro_engine_executor_post (h->executor, configure_action, o, g_free);
      library_settle (h);
      library_record (h, i, bro_json_value_new_string ("ok"));
      return;
    }
  if (strcmp (name, "advance") == 0)
    {
      manual_advance (h->clock, (gint64) (bro_json_value_get_number (arg, 0) * 1000), library_settle, h);
      library_settle (h);
      library_record (h, i, bro_json_value_new_string ("ok"));
    }
  else if (strcmp (name, "fs") == 0)
    {
      BroJsonValue *put = bro_json_value_member (arg, "put");
      const char *away = bro_json_value_get_string (bro_json_value_member (arg, "moveAway"), NULL);
      for (guint j = 0; put && j < put->keys->len; j++)
        tree_put (h->tree, put->keys->pdata[j], bro_json_value_get_string (put->items->pdata[j], ""));
      if (away)
        {
          g_autofree char *to = g_strconcat (away, ".away", NULL);
          tree_move (h->tree, away, to);
        }
      library_record (h, i, bro_json_value_new_string ("ok"));
    }
  else if (strcmp (name, "held") == 0)
    {
      library_settle (h);
      library_record (h, i, bro_json_value_new_integer (tree_held (h->tree)));
    }
  else if (strcmp (name, "release") == 0)
    {
      gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
      tree_release (h->tree);
      /* A woken call counts as held until it leaves the gate: wait for that, or the settle check passes early. */
      while (tree_held (h->tree) > 0)
        {
          if (g_get_monotonic_time () > until)
            g_error ("%s: the held calls didn't leave", library_case_id);
          g_usleep (1000);
        }
      for (guint j = 0; j < async->len; j++)
        library_wait_for (h, GPOINTER_TO_UINT (async->pdata[j]));
      library_settle (h);
      library_record (h, i, bro_json_value_new_string ("ok"));
    }
  else if (strcmp (name, "restart") == 0)
    {
      bro_engine_executor_post (h->executor, stop_action, h, NULL);
      library_settle (h);
      bro_engine_executor_post (h->executor, build_action, h, NULL);
      library_settle (h);
      library_record (h, i, bro_json_value_new_string ("ok"));
    }
  else if (strcmp (name, "dump") == 0)
    library_record (h, i, intents_json (h->db));
  else
    g_error ("unknown op %s", name);
  g_free (o);
}

static BroJsonValue *
tree_files_json (TestTree *t)
{
  BroJsonValue *o = bro_json_value_new_object ();
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, t->files);
  while (g_hash_table_iter_next (&it, &key, NULL))
    g_ptr_array_add (keys, key);
  g_ptr_array_sort (keys, compare_strings_in_array);
  for (guint i = 0; i < keys->len; i++)
    {
      GBytes *b = g_hash_table_lookup (t->files, keys->pdata[i]);
      g_autofree char *text = g_strndup (g_bytes_get_data (b, NULL), g_bytes_get_size (b));
      bro_json_value_set (o, keys->pdata[i], bro_json_value_new_string (text));
    }
  return o;
}

static BroJsonValue *
sorted_set_json (GHashTable *set)
{
  BroJsonValue *a = bro_json_value_new_array ();
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, set);
  while (g_hash_table_iter_next (&it, &key, NULL))
    g_ptr_array_add (keys, key);
  g_ptr_array_sort (keys, compare_strings_in_array);
  for (guint i = 0; i < keys->len; i++)
    bro_json_value_append (a, bro_json_value_new_string (keys->pdata[i]));
  return a;
}

static BroJsonValue *
time_or_null (gboolean has, BroInstant t)
{
  g_autofree char *text = has ? bro_instant_format (t) : NULL;
  return has ? bro_json_value_new_string (text) : bro_json_value_new_null ();
}

static BroJsonValue *
db_libraries_json (TestLibraryDb *db)
{
  g_autoptr (GPtrArray) all = db_all (BRO_LIBRARY_REPOSITORY (db), NULL);
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < all->len; i++)
    {
      BroLibraryRecord *l = all->pdata[i];
      BroJsonValue *o = bro_json_value_new_object ();
      bro_json_value_set (o, "id", bro_json_value_new_string (l->id));
      bro_json_value_set (o, "name", bro_json_value_new_string (l->name));
      bro_json_value_set (o, "path", bro_json_value_new_string (l->path));
      bro_json_value_set (o, "markerId", bro_json_value_new_string (l->marker_id));
      bro_json_value_set (o, "attachedAt", time_or_null (TRUE, l->attached_at));
      bro_json_value_set (o, "online", bro_json_value_new_bool (l->online));
      bro_json_value_set (o, "lastSeenAt", time_or_null (l->has_last_seen_at, l->last_seen_at));
      bro_json_value_append (a, o);
    }
  return a;
}

static BroJsonValue *
db_units_json (TestLibraryDb *db)
{
  BroJsonValue *a = bro_json_value_new_array ();
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, db->units);
  while (g_hash_table_iter_next (&it, &key, NULL))
    g_ptr_array_add (keys, key);
  g_ptr_array_sort (keys, compare_strings_in_array);
  for (guint i = 0; i < keys->len; i++)
    {
      BroUnitRecord *u = g_hash_table_lookup (db->units, keys->pdata[i]);
      BroJsonValue *o = bro_json_value_new_object ();
      bro_json_value_set (o, "id", bro_json_value_new_string (u->id.value));
      bro_json_value_set (o, "path", bro_json_value_new_string (u->path));
      bro_json_value_set (o, "state", bro_json_value_new_string (bro_unit_state_to_wire (u->state)));
      if (u->has_committed_at)
        bro_json_value_set (o, "committedAt", time_or_null (TRUE, u->committed_at));
      bro_json_value_append (a, o);
    }
  return a;
}

static BroJsonValue *
db_files_json (TestLibraryDb *db)
{
  BroJsonValue *o = bro_json_value_new_object ();
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, db->files);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      GPtrArray *files = value;
      BroJsonValue *a = bro_json_value_new_array ();
      for (guint i = 0; i < files->len; i++)
        {
          BroUnitFile *f = files->pdata[i];
          BroJsonValue *row = bro_json_value_new_array ();
          bro_json_value_append (row, bro_json_value_new_string (f->path));
          bro_json_value_append (row, bro_json_value_new_integer (f->size));
          bro_json_value_append (row, bro_json_value_new_string (f->sha256));
          bro_json_value_append (row, bro_json_value_new_string (f->role));
          bro_json_value_append (a, row);
        }
      bro_json_value_set (o, key, a);
    }
  return o;
}

static void
gather_action (gpointer data)
{
  LibraryOp *o = data;
  BroJsonValue *r = bro_json_value_new_object ();
  bro_json_value_set (r, "events", events_json (o->h->events));
  bro_json_value_set (r, "problems", problems_json (o->h->problems));
  library_record (o->h, o->index, r);
}

static void
library_teardown_action (gpointer data)
{
  LibraryHarness *h = data;
  bro_library_service_stop (h->service);
  if (h->subscription)
    bro_subscription_cancel (h->subscription);
}

static gboolean
library_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  LibraryHarness h = { 0 };
  BroJsonValue *ops = bro_json_value_member (given, "ops");
  BroJsonValue *list;
  g_autoptr (GPtrArray) async = g_ptr_array_new ();
  g_autoptr (BroJsonValue) results = bro_json_value_new_array ();
  g_autoptr (BroJsonValue) gathered = NULL;
  BroJsonValue *want;
  LibraryOp *gather;
  library_case_id = id;
  g_mutex_init (&h.lock);
  g_cond_init (&h.cond);
  h.clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  h.executor = bro_engine_executor_new ("library-test");
  h.engine_clock = bro_engine_clock_new (BRO_CLOCK (h.clock), h.executor);
  h.tree = g_object_new (TEST_TYPE_TREE, NULL);
  h.db = g_object_new (TEST_TYPE_LIBRARY_DB, NULL);
  h.config = g_object_new (TEST_TYPE_CASE_CONFIG, NULL);
  h.events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  h.n_ops = bro_json_value_length (ops);
  h.results = g_new0 (BroJsonValue *, h.n_ops + 1);
  h.done = g_new0 (gboolean, h.n_ops + 1);
  h.crash_at = bro_json_value_get_string (bro_json_value_member (given, "crashAt"), NULL);
  bro_json_value_set (h.config->document, "version", bro_json_value_new_integer (3));
  list = bro_json_value_member (given, "config");
  bro_json_value_set (h.config->document, "libraries", list ? bro_json_value_ref (list) : bro_json_value_new_array ());
  list = bro_json_value_member (given, "dirs");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    tree_make_dirs (h.tree, bro_json_value_get_string (bro_json_value_at (list, i), ""));
  list = bro_json_value_member (given, "files");
  for (guint i = 0; list && i < list->keys->len; i++)
    tree_put (h.tree, list->keys->pdata[i], bro_json_value_get_string (list->items->pdata[i], ""));
  list = bro_json_value_member (given, "fail");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *f = bro_json_value_at (list, i);
      TreeFail *tf = g_new0 (TreeFail, 1);
      tf->op = g_strdup (bro_json_value_get_string (bro_json_value_member (f, "op"), ""));
      tf->path = g_strdup (bro_json_value_get_string (bro_json_value_member (f, "path"), ""));
      tf->error = bro_json_value_ref (bro_json_value_member (f, "error"));
      tf->times = (int) bro_json_value_get_integer (bro_json_value_member (f, "times"), -1);
      g_ptr_array_add (h.tree->fails, tf);
    }
  list = bro_json_value_member (given, "hold");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *x = bro_json_value_at (list, i);
      g_ptr_array_add (h.tree->holds, g_strconcat (bro_json_value_get_string (bro_json_value_member (x, "op"), ""), " ",
                                                   bro_json_value_get_string (bro_json_value_member (x, "path"), ""), NULL));
    }
  list = bro_json_value_member (given, "libraries");
  for (guint i = 0; list && i < bro_json_value_length (list); i++)
    {
      BroJsonValue *l = bro_json_value_at (list, i);
      BroLibraryRecord *r = bro_library_record_new ();
      const char *seen = bro_json_value_get_string (bro_json_value_member (l, "lastSeenAt"), NULL);
      r->id = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "id"), ""));
      r->name = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "name"), ""));
      r->path = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "path"), ""));
      r->marker_id = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "markerId"), ""));
      bro_instant_parse (bro_json_value_get_string (bro_json_value_member (l, "attachedAt"), ""), &r->attached_at);
      r->online = bro_json_value_get_bool (bro_json_value_member (l, "online"), FALSE);
      r->has_last_seen_at = seen && bro_instant_parse (seen, &r->last_seen_at);
      g_hash_table_replace (h.db->libraries, g_strdup (r->id), r);
    }
  bro_engine_executor_post (h.executor, build_action, &h, NULL);
  library_settle (&h);

  for (guint i = 0; i < h.n_ops; i++)
    run_library_op (&h, i, bro_json_value_at (ops, i), async);
  for (guint j = 0; j < async->len; j++)
    library_wait_for (&h, GPOINTER_TO_UINT (async->pdata[j]));
  library_settle (&h);

  for (guint i = 0; i < h.n_ops; i++)
    bro_json_value_append (results, h.results[i] ? bro_json_value_ref (h.results[i]) : bro_json_value_new_null ());
  bro_test_same_json (failures, id, "results", bro_json_value_member (expect, "results"), results);
  gather = g_new0 (LibraryOp, 1);
  gather->h = &h;
  gather->index = h.n_ops;
  bro_engine_executor_post (h.executor, gather_action, gather, g_free);
  library_wait_for (&h, h.n_ops);
  gathered = bro_json_value_ref (h.results[h.n_ops]);
  if ((want = bro_json_value_member (expect, "events")))
    same_sorted (failures, id, "events", want, bro_json_value_member (gathered, "events"));
  if ((want = bro_json_value_member (expect, "problems")))
    same_sorted (failures, id, "problems", want, bro_json_value_member (gathered, "problems"));
  if ((want = bro_json_value_member (expect, "files")))
    {
      g_autoptr (BroJsonValue) got = tree_files_json (h.tree);
      same_sorted (failures, id, "files", want, got);
    }
  if ((want = bro_json_value_member (expect, "modes")))
    for (guint i = 0; i < want->keys->len; i++)
      {
        g_autoptr (BroJsonValue) got = bro_json_value_new_integer (GPOINTER_TO_INT (g_hash_table_lookup (h.tree->modes, want->keys->pdata[i])));
        bro_test_same_json (failures, id, "mode", want->items->pdata[i], got);
      }
  if ((want = bro_json_value_member (expect, "dirs")))
    {
      g_autoptr (BroJsonValue) got = sorted_set_json (h.tree->dirs);
      bro_test_same_json (failures, id, "dirs", want, got);
    }
  if ((want = bro_json_value_member (expect, "synced")))
    {
      g_autoptr (BroJsonValue) got = sorted_set_json (h.tree->synced);
      bro_test_same_json (failures, id, "synced", want, got);
    }
  if ((want = bro_json_value_member (expect, "db")))
    {
      BroJsonValue *part;
      if ((part = bro_json_value_member (want, "libraries")))
        {
          g_autoptr (BroJsonValue) got = db_libraries_json (h.db);
          same_sorted (failures, id, "db libraries", part, got);
        }
      if ((part = bro_json_value_member (want, "units")))
        {
          g_autoptr (BroJsonValue) got = db_units_json (h.db);
          same_sorted (failures, id, "db units", part, got);
        }
      if ((part = bro_json_value_member (want, "intents")))
        {
          g_autoptr (BroJsonValue) got = intents_json (h.db);
          bro_test_same_json (failures, id, "db intents", part, got);
        }
      if ((part = bro_json_value_member (want, "unitFiles")))
        {
          g_autoptr (BroJsonValue) got = db_files_json (h.db);
          same_sorted (failures, id, "db unit files", part, got);
        }
    }

  bro_engine_executor_post (h.executor, library_teardown_action, &h, NULL);
  bro_engine_executor_stop (h.executor);
  g_clear_object (&h.service);
  g_clear_object (&h.subscription);
  g_clear_object (&h.scheduler);
  g_clear_object (&h.problems);
  g_clear_object (&h.bus);
  g_object_unref (h.engine_clock);
  g_object_unref (h.executor);
  g_object_unref (h.clock);
  g_object_unref (h.tree);
  g_object_unref (h.db);
  g_object_unref (h.config);
  g_ptr_array_unref (h.events);
  for (guint i = 0; i <= h.n_ops; i++)
    if (h.results[i])
      bro_json_value_unref (h.results[i]);
  g_free (h.results);
  g_free (h.done);
  g_mutex_clear (&h.lock);
  g_cond_clear (&h.cond);
  return TRUE;
}

static void
test_library_service (void)
{
  bro_test_run_cases ("engine/library-service.cases.json", library_case);
}

static void
test_sha256_hasher (void)
{
  g_autoptr (BroSha256Hasher) h = bro_sha256_hasher_new ();
  g_autofree char *digest = NULL;
  bro_sha256_hasher_update (h, (const guint8 *) "a", 1);
  bro_sha256_hasher_update (h, (const guint8 *) "", 0);
  bro_sha256_hasher_update (h, (const guint8 *) "bc", 2);
  digest = bro_sha256_hasher_finish (h);
  g_assert_cmpstr (digest, ==, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
