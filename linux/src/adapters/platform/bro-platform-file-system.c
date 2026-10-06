/* bro-platform-file-system.c */
#define _GNU_SOURCE /* renameat2 */
#include "bro-platform-file-system.h"

#include "bro-conflict-namer.h"
#include "bro-message-code.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif

/* ---- errors ---- */

static void
set_path_error (BroBroError **error, BroMessageCode code, const char *path)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_bro_error_set (error, bro_message_code_wire (code), params);
}

static void
set_failed (BroBroError **error, const char *operation, const char *path, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string (operation));
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_FAILED), params);
}

/* fs.notFound for ENOENT, else fs.failed with the system's text. Always FALSE. */
static gboolean
set_errno_error (BroBroError **error, int code, const char *operation, const char *path)
{
  if (code == ENOENT)
    set_path_error (error, BRO_MSG_FS_NOT_FOUND, path);
  else
    set_failed (error, operation, path, g_strerror (code));
  return FALSE;
}

/* ---- small helpers ---- */

static gboolean
is_directory (const char *path)
{
  struct stat st;
  return stat (path, &st) == 0 && S_ISDIR (st.st_mode);
}

static gboolean
exists (const char *path)
{
  struct stat st;
  return lstat (path, &st) == 0;
}

static gboolean
full_sync (int fd)
{
#ifdef F_FULLFSYNC
  if (fcntl (fd, F_FULLFSYNC) == 0)
    return TRUE;
#endif
  return fsync (fd) == 0;
}

static int
open_file (const char *path, const char *operation, BroBroError **error)
{
  int fd;
  if (is_directory (path))
    {
      set_failed (error, operation, path, "it is a folder");
      return -1;
    }
  fd = open (path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    set_errno_error (error, errno, operation, path);
  return fd;
}

static gboolean
same_ignoring_ascii_case (const char *a, const char *b)
{
  return g_ascii_strcasecmp (a, b) == 0;
}

/* ---- the byte stream ---- */

#define BRO_TYPE_FILE_BYTE_STREAM (bro_file_byte_stream_get_type ())
G_DECLARE_FINAL_TYPE (BroFileByteStream, bro_file_byte_stream, BRO, FILE_BYTE_STREAM, GObject)

struct _BroFileByteStream {
  GObject parent_instance;
  GMutex lock;
  int fd;
  char *path;
};

static void bro_file_byte_stream_iface_init (BroByteStreamInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroFileByteStream, bro_file_byte_stream, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_BYTE_STREAM, bro_file_byte_stream_iface_init))

static GBytes *
stream_read (BroByteStream *stream, int max_bytes, BroBroError **error)
{
  BroFileByteStream *self = BRO_FILE_BYTE_STREAM (stream);
  guint8 *buffer = g_malloc (MAX (max_bytes, 1));
  ssize_t n = 0;
  g_mutex_lock (&self->lock);
  if (self->fd >= 0)
    while ((n = read (self->fd, buffer, max_bytes)) < 0 && errno == EINTR)
      ;
  g_mutex_unlock (&self->lock);
  if (n < 0)
    {
      set_failed (error, "read", self->path, g_strerror (errno));
      g_free (buffer);
      return NULL;
    }
  return g_bytes_new_take (buffer, (gsize) n);
}

static void
stream_close (BroByteStream *stream)
{
  BroFileByteStream *self = BRO_FILE_BYTE_STREAM (stream);
  g_mutex_lock (&self->lock);
  if (self->fd >= 0)
    close (self->fd);
  self->fd = -1;
  g_mutex_unlock (&self->lock);
}

static void
bro_file_byte_stream_finalize (GObject *object)
{
  BroFileByteStream *self = BRO_FILE_BYTE_STREAM (object);
  if (self->fd >= 0)
    close (self->fd);
  g_free (self->path);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_file_byte_stream_parent_class)->finalize (object);
}

static void
bro_file_byte_stream_class_init (BroFileByteStreamClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_file_byte_stream_finalize;
}

static void
bro_file_byte_stream_init (BroFileByteStream *self)
{
  g_mutex_init (&self->lock);
  self->fd = -1;
}

static void
bro_file_byte_stream_iface_init (BroByteStreamInterface *iface)
{
  iface->read = stream_read;
  iface->close = stream_close;
}

/* ---- the file system ---- */

struct _BroPlatformFileSystem {
  GObject parent_instance;
};

static void bro_platform_file_system_iface_init (BroFileSystemInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformFileSystem, bro_platform_file_system, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, bro_platform_file_system_iface_init))

gboolean
bro_platform_file_system_exists (BroPlatformFileSystem *self, const char *path)
{
  return exists (path);
}

BroFileInfo *
bro_platform_file_system_stat (BroPlatformFileSystem *self, const char *path, BroBroError **error)
{
  struct stat st;
  BroFileInfo *info;
  if (stat (path, &st) != 0)
    {
      set_errno_error (error, errno, "stat", path);
      return NULL;
    }
  info = bro_file_info_new ();
  info->is_directory = S_ISDIR (st.st_mode);
  info->size = info->is_directory ? 0 : (gint64) st.st_size;
#ifdef __APPLE__
  info->modified_at.unix_milliseconds = (gint64) st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
  info->modified_at.unix_milliseconds = (gint64) st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
  return info;
}

GPtrArray *
bro_platform_file_system_list (BroPlatformFileSystem *self, const char *directory, BroBroError **error)
{
  DIR *dir = opendir (directory);
  struct dirent *e;
  GPtrArray *entries;
  if (!dir)
    {
      set_errno_error (error, errno, "list", directory);
      return NULL;
    }
  entries = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_directory_entry_free);
  while ((e = readdir (dir)) != NULL)
    {
      BroDirectoryEntry *entry;
      if (g_str_equal (e->d_name, ".") || g_str_equal (e->d_name, ".."))
        continue;
      entry = bro_directory_entry_new ();
      entry->name = g_strdup (e->d_name);
      if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
        {
          g_autofree char *full = g_build_filename (directory, e->d_name, NULL);
          entry->is_directory = is_directory (full);
        }
      else
        entry->is_directory = e->d_type == DT_DIR;
      g_ptr_array_add (entries, entry);
    }
  closedir (dir);
  return entries;
}

GBytes *
bro_platform_file_system_read (BroPlatformFileSystem *self, const char *path, BroBroError **error)
{
  GByteArray *out;
  guint8 buffer[65536];
  int fd = open_file (path, "read", error);
  if (fd < 0)
    return NULL;
  out = g_byte_array_new ();
  for (;;)
    {
      ssize_t n = read (fd, buffer, sizeof buffer);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        {
          set_errno_error (error, errno, "read", path);
          g_byte_array_unref (out);
          close (fd);
          return NULL;
        }
      if (n == 0)
        break;
      g_byte_array_append (out, buffer, (guint) n);
    }
  close (fd);
  return g_byte_array_free_to_bytes (out);
}

GBytes *
bro_platform_file_system_read_range (BroPlatformFileSystem *self, const char *path, gint64 offset, int length,
                                     BroBroError **error)
{
  int fd = open_file (path, "read", error);
  guint8 *buffer;
  int got = 0;
  if (fd < 0)
    return NULL;
  buffer = g_malloc (MAX (length, 1));
  while (got < length)
    {
      ssize_t n = pread (fd, buffer + got, (size_t) (length - got), (off_t) (offset + got));
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        {
          set_errno_error (error, errno, "read", path);
          g_free (buffer);
          close (fd);
          return NULL;
        }
      if (n == 0)
        break;
      got += (int) n;
    }
  close (fd);
  return g_bytes_new_take (buffer, (gsize) got);
}

gboolean
bro_platform_file_system_create_directory (BroPlatformFileSystem *self, const char *path, gboolean parents_must_exist,
                                           BroBroError **error)
{
  g_autofree char *parent = NULL;
  if (is_directory (path))
    return TRUE;
  if (exists (path))
    {
      set_path_error (error, BRO_MSG_FS_ALREADY_EXISTS, path);
      return FALSE;
    }
  parent = g_path_get_dirname (path);
  if (!is_directory (parent))
    {
      if (parents_must_exist)
        {
          set_path_error (error, BRO_MSG_FS_PARENT_MISSING, parent);
          return FALSE;
        }
      if (!bro_platform_file_system_create_directory (self, parent, FALSE, error))
        return FALSE;
    }
  if (mkdir (path, 0777) != 0 && !(errno == EEXIST && is_directory (path)))
    return set_errno_error (error, errno, "createDirectory", path);
  return TRUE;
}

gboolean
bro_platform_file_system_sync_directory (BroPlatformFileSystem *self, const char *path, BroBroError **error)
{
  int fd = open (path, O_RDONLY | O_CLOEXEC);
  gboolean ok;
  if (fd < 0)
    return set_errno_error (error, errno, "sync", path);
  ok = full_sync (fd);
  /* A file system that can't sync a folder at all has nothing to do: the rename is already its to keep. */
  if (!ok && (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP))
    ok = TRUE;
  if (!ok)
    set_errno_error (error, errno, "sync", path);
  close (fd);
  return ok;
}

gboolean
bro_platform_file_system_write_atomically (BroPlatformFileSystem *self, const char *path, GBytes *bytes, int mode,
                                           BroBroError **error)
{
  g_autofree char *parent = g_path_get_dirname (path);
  g_autofree char *base = g_path_get_basename (path);
  g_autofree char *temp = NULL;
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  gsize written = 0;
  int fd;
  if (!is_directory (parent))
    {
      set_path_error (error, BRO_MSG_FS_PARENT_MISSING, parent);
      return FALSE;
    }
  temp = g_strdup_printf ("%s/.%s.bromelia-tmp-%08x", parent, base, g_random_int ());
  fd = open (temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t) mode);
  if (fd < 0)
    return set_errno_error (error, errno, "write", path);
  while (written < size)
    {
      ssize_t n = write (fd, data + written, size - written);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        {
          set_errno_error (error, errno, "write", path);
          close (fd);
          unlink (temp);
          return FALSE;
        }
      written += (gsize) n;
    }
  fchmod (fd, (mode_t) mode); /* whatever the umask took away */
  if (!full_sync (fd))
    {
      set_errno_error (error, errno, "write", path);
      close (fd);
      unlink (temp);
      return FALSE;
    }
  close (fd);
  if (rename (temp, path) != 0)
    {
      set_errno_error (error, errno, "write", path);
      unlink (temp);
      return FALSE;
    }
  return bro_platform_file_system_sync_directory (self, parent, error);
}

/* rename(2) that never replaces: a file that appears meanwhile is kept. */
static int
rename_no_replace (const char *from, const char *to)
{
#if defined(__APPLE__)
  /* macOS's SMB client has no RENAME_EXCL (ENOTSUP). */
  int rc = renamex_np (from, to, RENAME_EXCL);
  if (rc == 0 || (errno != ENOTSUP && errno != EINVAL))
    return rc;
#elif defined(__linux__) && defined(RENAME_NOREPLACE)
  int rc = renameat2 (AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
  if (rc == 0 || (errno != EINVAL && errno != ENOSYS))
    return rc;
#endif
#if defined(__APPLE__) || (defined(__linux__) && defined(RENAME_NOREPLACE))
  /* A file system without an exclusive rename: link + unlink never replaces either (files only); where links aren't
   * offered either, a check that nothing is there, then rename (Bromelia's names are unique to it). */
  if (!is_directory (from) && link (from, to) == 0)
    return unlink (from);
  if (errno == EEXIST)
    return -1;
  if (exists (to))
    {
      errno = EEXIST;
      return -1;
    }
  return rename (from, to);
#else
  if (exists (to))
    {
      errno = EEXIST;
      return -1;
    }
  return rename (from, to);
#endif
}

gboolean
bro_platform_file_system_rename (BroPlatformFileSystem *self, const char *from, const char *to, BroBroError **error)
{
  g_autofree char *parent = NULL;
  if (!exists (from))
    {
      set_path_error (error, BRO_MSG_FS_NOT_FOUND, from);
      return FALSE;
    }
  if (exists (to))
    {
      set_path_error (error, BRO_MSG_FS_ALREADY_EXISTS, to);
      return FALSE;
    }
  parent = g_path_get_dirname (to);
  if (!is_directory (parent))
    {
      set_path_error (error, BRO_MSG_FS_PARENT_MISSING, parent);
      return FALSE;
    }
  if (rename_no_replace (from, to) != 0)
    {
      if (errno == EEXIST)
        set_path_error (error, BRO_MSG_FS_ALREADY_EXISTS, to);
      else
        set_errno_error (error, errno, "rename", from);
      return FALSE;
    }
  return TRUE;
}

gboolean
bro_platform_file_system_remove (BroPlatformFileSystem *self, const char *path, BroBroError **error)
{
  if (is_directory (path))
    {
      if (rmdir (path) == 0)
        return TRUE;
      if (errno == ENOTEMPTY || errno == EEXIST)
        set_failed (error, "remove", path, "the folder isn't empty");
      else
        set_errno_error (error, errno, "remove", path);
      return FALSE;
    }
  if (!exists (path))
    {
      set_path_error (error, BRO_MSG_FS_NOT_FOUND, path);
      return FALSE;
    }
  if (unlink (path) != 0)
    return set_errno_error (error, errno, "remove", path);
  return TRUE;
}

/* The names in @entries (BroDirectoryEntry *), NULL-terminated; free the array only (the names are borrowed). */
static const char **
entry_names (GPtrArray *entries)
{
  const char **names = g_new0 (const char *, entries->len + 1);
  for (guint i = 0; i < entries->len; i++)
    names[i] = ((BroDirectoryEntry *) entries->pdata[i])->name;
  return names;
}

static int
compare_entries (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

/* Every file under @dir, as "/relative/path", appended to @out. */
static gboolean
files_under (BroPlatformFileSystem *self, const char *dir, const char *prefix, GPtrArray *out, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = bro_platform_file_system_list (self, dir, error);
  if (!entries)
    return FALSE;
  for (guint i = 0; i < entries->len; i++)
    {
      BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *rel = g_strdup_printf ("%s/%s", prefix, e->name);
      if (e->is_directory)
        {
          g_autofree char *sub = g_build_filename (dir, e->name, NULL);
          if (!files_under (self, sub, rel, out, error))
            return FALSE;
        }
      else
        g_ptr_array_add (out, g_steal_pointer (&rel));
    }
  return TRUE;
}

/* What moveMerging has moved so far, and who hears of each item. */
typedef struct {
  GPtrArray *items; /* BroMovedItem * */
  BroMovedFunc on_moved;
  gpointer data;
} Moves;

static void
add_moved (Moves *moved, const char *from, const char *to)
{
  BroMovedItem *item = bro_moved_item_new ();
  item->from = g_strdup (from);
  item->to = g_strdup (to);
  g_ptr_array_add (moved->items, item);
  if (moved->on_moved)
    moved->on_moved (item, moved->data);
}

/* Each entry of @source in turn: a folder merges into a folder of the same name (ignoring ASCII case); anything else
 * moves under ConflictNamer's name. Emptied source folders go. */
static gboolean
merge (BroPlatformFileSystem *self, const char *source, const char *target, Moves *moved, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = bro_platform_file_system_list (self, source, error);
  if (!entries)
    return FALSE;
  g_ptr_array_sort (entries, compare_entries);
  for (guint i = 0; i < entries->len; i++)
    {
      BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *src = g_build_filename (source, e->name, NULL);
      g_autoptr (GPtrArray) existing = bro_platform_file_system_list (self, target, error);
      BroDirectoryEntry *same = NULL;
      g_autofree const char **names = NULL;
      g_autofree char *name = NULL;
      g_autofree char *dst = NULL;
      if (!existing)
        return FALSE;
      for (guint j = 0; j < existing->len && !same; j++)
        if (same_ignoring_ascii_case (((BroDirectoryEntry *) existing->pdata[j])->name, e->name))
          same = existing->pdata[j];
      if (e->is_directory && same && same->is_directory)
        {
          g_autofree char *into = g_build_filename (target, same->name, NULL);
          if (!merge (self, src, into, moved, error) || !bro_platform_file_system_remove (self, src, error))
            return FALSE;
          continue;
        }
      names = entry_names (existing);
      name = bro_conflict_namer_next (e->name, names, e->is_directory);
      dst = g_build_filename (target, name, NULL);
      if (!bro_platform_file_system_rename (self, src, dst, error))
        return FALSE;
      if (e->is_directory)
        {
          g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
          if (!files_under (self, dst, "", files, error))
            return FALSE;
          for (guint j = 0; j < files->len; j++)
            {
              g_autofree char *f = g_strconcat (src, files->pdata[j], NULL);
              g_autofree char *t = g_strconcat (dst, files->pdata[j], NULL);
              add_moved (moved, f, t);
            }
        }
      else
        add_moved (moved, src, dst);
    }
  return TRUE;
}

static int
compare_moved (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroMovedItem *const *) a)->from, (*(BroMovedItem *const *) b)->from);
}

GPtrArray *
bro_platform_file_system_move_merging (BroPlatformFileSystem *self, const char *from, const char *to, BroMovePolicy policy,
                                       BroMovedFunc on_moved, gpointer data, BroBroError **error)
{
  Moves moved;
  if (!is_directory (from))
    {
      if (exists (from))
        set_failed (error, "move", from, "it isn't a folder");
      else
        set_path_error (error, BRO_MSG_FS_NOT_FOUND, from);
      return NULL;
    }
  if (!exists (to))
    {
      if (!bro_platform_file_system_create_directory (self, to, TRUE, error))
        return NULL;
    }
  else if (!is_directory (to))
    {
      set_path_error (error, BRO_MSG_FS_ALREADY_EXISTS, to);
      return NULL;
    }
  moved = (Moves) { g_ptr_array_new_with_free_func ((GDestroyNotify) bro_moved_item_free), on_moved, data };
  if (!merge (self, from, to, &moved, error))
    {
      g_ptr_array_unref (moved.items);
      return NULL;
    }
  g_ptr_array_sort (moved.items, compare_moved);
  return moved.items;
}

gboolean
bro_platform_file_system_move_to_trash (BroPlatformFileSystem *self, const char *path, const char *trash, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = NULL;
  g_autofree const char **names = NULL;
  g_autofree char *base = NULL;
  g_autofree char *name = NULL;
  g_autofree char *dst = NULL;
  if (!exists (path))
    {
      set_path_error (error, BRO_MSG_FS_NOT_FOUND, path);
      return FALSE;
    }
  if (!bro_platform_file_system_create_directory (self, trash, TRUE, error))
    return FALSE;
  entries = bro_platform_file_system_list (self, trash, error);
  if (!entries)
    return FALSE;
  names = entry_names (entries);
  base = g_path_get_basename (path);
  name = bro_conflict_namer_next (base, names, is_directory (path));
  dst = g_build_filename (trash, name, NULL);
  return bro_platform_file_system_rename (self, path, dst, error);
}

gboolean
bro_platform_file_system_sync_file (BroPlatformFileSystem *self, const char *path, BroBroError **error)
{
  int fd = open_file (path, "sync", error);
  gboolean ok;
  if (fd < 0)
    return FALSE;
  ok = full_sync (fd);
  if (!ok)
    set_errno_error (error, errno, "sync", path);
  close (fd);
  return ok;
}

BroByteStream *
bro_platform_file_system_open_for_reading (BroPlatformFileSystem *self, const char *path, gboolean bypass_cache,
                                           BroBroError **error)
{
  BroFileByteStream *stream;
  int fd = open_file (path, "read", error);
  if (fd < 0)
    return NULL;
  if (bypass_cache)
    {
#ifdef F_NOCACHE
      fcntl (fd, F_NOCACHE, 1);
#else
      /* Drop the file's clean pages, so what follows is read from the disk, not from memory. */
      posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    }
  stream = g_object_new (BRO_TYPE_FILE_BYTE_STREAM, NULL);
  stream->fd = fd;
  stream->path = g_strdup (path);
  return BRO_BYTE_STREAM (stream);
}

#ifndef __APPLE__
/* statfs's f_type as a name, for the file systems a library may be on. */
static const char *
fs_type_name (long type)
{
  switch ((unsigned long) type)
    {
    case 0xEF53: return "ext4";
    case 0x9123683EUL: return "btrfs";
    case 0x58465342: return "xfs";
    case 0x2FC12FC1: return "zfs";
    case 0x01021994: return "tmpfs";
    case 0x4d44: return "vfat";
    case 0x2011BAB0: return "exfat";
    case 0x5346544e: return "ntfs";
    case 0x7366746e: return "ntfs3";
    case 0x65735546: return "fuse";
    case 0xFF534D42: return "cifs";
    case 0xFE534D42: return "smb2";
    case 0x6969: return "nfs";
    case 0x794c7630: return "overlay";
    case 0x6a656a63: return "fakeowner";
    case 0x15013346: return "udf";
    case 0x9660: return "iso9660";
    default: return "unknown";
    }
}
#endif

BroVolumeInfo *
bro_platform_file_system_volume (BroPlatformFileSystem *self, const char *path, BroBroError **error)
{
  g_autofree char *existing = g_strdup (path);
  struct statfs fs;
  BroVolumeInfo *v;
  guint32 id[2];
  while (!exists (existing) && strcmp (existing, "/") != 0 && *existing)
    {
      char *up = g_path_get_dirname (existing);
      g_free (existing);
      existing = up;
    }
  if (statfs (existing, &fs) != 0)
    {
      set_errno_error (error, errno, "volume", path);
      return NULL;
    }
  memcpy (id, &fs.f_fsid, sizeof id);
  v = bro_volume_info_new ();
  v->id = g_strdup_printf ("%08x%08x", id[0], id[1]);
  v->free_bytes = (gint64) fs.f_bavail * (gint64) fs.f_bsize;
#ifdef __APPLE__
  v->fs_type = g_strdup (fs.f_fstypename);
  v->case_sensitive = pathconf (existing, _PC_CASE_SENSITIVE) == 1;
#else
  v->fs_type = g_strdup (fs_type_name ((long) fs.f_type));
  v->case_sensitive = !(g_str_equal (v->fs_type, "vfat") || g_str_equal (v->fs_type, "exfat") || g_str_equal (v->fs_type, "ntfs")
                        || g_str_equal (v->fs_type, "ntfs3") || g_str_equal (v->fs_type, "cifs") || g_str_equal (v->fs_type, "smb2"));
#endif
  return v;
}

/* ---- the port ---- */

static gboolean
exists_vfunc (BroFileSystem *self, const char *path)
{
  return bro_platform_file_system_exists (BRO_PLATFORM_FILE_SYSTEM (self), path);
}

static BroFileInfo *
stat_vfunc (BroFileSystem *self, const char *path, BroBroError **error)
{
  return bro_platform_file_system_stat (BRO_PLATFORM_FILE_SYSTEM (self), path, error);
}

static GPtrArray *
list_vfunc (BroFileSystem *self, const char *directory, BroBroError **error)
{
  return bro_platform_file_system_list (BRO_PLATFORM_FILE_SYSTEM (self), directory, error);
}

static GBytes *
read_vfunc (BroFileSystem *self, const char *path, BroBroError **error)
{
  return bro_platform_file_system_read (BRO_PLATFORM_FILE_SYSTEM (self), path, error);
}

static GBytes *
read_range_vfunc (BroFileSystem *self, const char *path, gint64 offset, int length, BroBroError **error)
{
  return bro_platform_file_system_read_range (BRO_PLATFORM_FILE_SYSTEM (self), path, offset, length, error);
}

static gboolean
create_directory_vfunc (BroFileSystem *self, const char *path, gboolean parents_must_exist, BroBroError **error)
{
  return bro_platform_file_system_create_directory (BRO_PLATFORM_FILE_SYSTEM (self), path, parents_must_exist, error);
}

static gboolean
write_atomically_vfunc (BroFileSystem *self, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  return bro_platform_file_system_write_atomically (BRO_PLATFORM_FILE_SYSTEM (self), path, bytes, mode, error);
}

static gboolean
rename_vfunc (BroFileSystem *self, const char *from, const char *to, BroBroError **error)
{
  return bro_platform_file_system_rename (BRO_PLATFORM_FILE_SYSTEM (self), from, to, error);
}

static GPtrArray *
move_merging_vfunc (BroFileSystem *self, const char *from, const char *to, BroMovePolicy policy, BroMovedFunc on_moved, gpointer data,
                    BroBroError **error)
{
  return bro_platform_file_system_move_merging (BRO_PLATFORM_FILE_SYSTEM (self), from, to, policy, on_moved, data, error);
}

static gboolean
remove_vfunc (BroFileSystem *self, const char *path, BroBroError **error)
{
  return bro_platform_file_system_remove (BRO_PLATFORM_FILE_SYSTEM (self), path, error);
}

static gboolean
move_to_trash_vfunc (BroFileSystem *self, const char *path, const char *trash, BroBroError **error)
{
  return bro_platform_file_system_move_to_trash (BRO_PLATFORM_FILE_SYSTEM (self), path, trash, error);
}

static gboolean
sync_file_vfunc (BroFileSystem *self, const char *path, BroBroError **error)
{
  return bro_platform_file_system_sync_file (BRO_PLATFORM_FILE_SYSTEM (self), path, error);
}

static gboolean
sync_directory_vfunc (BroFileSystem *self, const char *path, BroBroError **error)
{
  return bro_platform_file_system_sync_directory (BRO_PLATFORM_FILE_SYSTEM (self), path, error);
}

static BroByteStream *
open_for_reading_vfunc (BroFileSystem *self, const char *path, gboolean bypass_cache, BroBroError **error)
{
  return bro_platform_file_system_open_for_reading (BRO_PLATFORM_FILE_SYSTEM (self), path, bypass_cache, error);
}

static BroVolumeInfo *
volume_vfunc (BroFileSystem *self, const char *path, BroBroError **error)
{
  return bro_platform_file_system_volume (BRO_PLATFORM_FILE_SYSTEM (self), path, error);
}

static void
bro_platform_file_system_class_init (BroPlatformFileSystemClass *klass)
{
}

static void
bro_platform_file_system_init (BroPlatformFileSystem *self)
{
}

static void
bro_platform_file_system_iface_init (BroFileSystemInterface *iface)
{
  iface->exists = exists_vfunc;
  iface->stat = stat_vfunc;
  iface->list = list_vfunc;
  iface->read = read_vfunc;
  iface->read_range = read_range_vfunc;
  iface->create_directory = create_directory_vfunc;
  iface->write_atomically = write_atomically_vfunc;
  iface->rename = rename_vfunc;
  iface->move_merging = move_merging_vfunc;
  iface->remove = remove_vfunc;
  iface->move_to_trash = move_to_trash_vfunc;
  iface->sync_file = sync_file_vfunc;
  iface->sync_directory = sync_directory_vfunc;
  iface->open_for_reading = open_for_reading_vfunc;
  iface->volume = volume_vfunc;
}

BroPlatformFileSystem *
bro_platform_file_system_new (void)
{
  return g_object_new (BRO_TYPE_PLATFORM_FILE_SYSTEM, NULL);
}
