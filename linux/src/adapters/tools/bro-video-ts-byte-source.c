/* bro-video-ts-byte-source.c */
#include "bro-video-ts-byte-source.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR G_GINT64_CONSTANT (2048)

struct _BroVideoTsByteSource {
  BroByteSource base;
  int image;          /* an image's descriptor, or -1 for a folder */
  GHashTable *lba;    /* name → gint64 * (an image) */
  GHashTable *paths;  /* name → path (a folder) */
  GHashTable *sizes;  /* name → gint64 * */
  char *label;
};

static GBytes *
read_at (int fd, gint64 offset, gsize count)
{
  guint8 *buffer = g_malloc (count ? count : 1);
  gsize total = 0;
  while (total < count)
    {
      ssize_t n = pread (fd, buffer + total, count - total, (off_t) (offset + (gint64) total));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      total += (gsize) n;
    }
  return g_bytes_new_take (buffer, total);
}

/* Whole sectors around the range, then the range: a disc device only reads whole sectors. */
static GBytes *
read_sectors (int fd, gint64 offset, gsize count)
{
  gint64 start = offset / SECTOR * SECTOR, end = (offset + (gint64) count + SECTOR - 1) / SECTOR * SECTOR;
  g_autoptr (GBytes) data = read_at (fd, start, (gsize) (end - start));
  gsize skip = (gsize) (offset - start), size = g_bytes_get_size (data);
  if (size <= skip)
    return g_bytes_new (NULL, 0);
  return g_bytes_new_from_bytes (data, skip, MIN (size - skip, count));
}

static guint32
u32 (const guint8 *b, gsize size, gsize i)
{
  return i + 4 <= size ? (guint32) b[i] | (guint32) b[i + 1] << 8 | (guint32) b[i + 2] << 16 | (guint32) b[i + 3] << 24 : 0;
}

typedef struct {
  char *name;
  gint64 lba, size;
} Entry;

static void
entry_free (gpointer data)
{
  g_free (((Entry *) data)->name);
  g_free (data);
}

/* The entries of an ISO 9660 directory: name (without ";1"), first sector, size. */
static GPtrArray *
directory (int fd, gint64 lba, gint64 size)
{
  g_autoptr (GBytes) bytes = read_sectors (fd, lba * SECTOR, (gsize) MIN (size, 1 << 24));
  gsize n = 0, i = 0;
  const guint8 *data = g_bytes_get_data (bytes, &n);
  GPtrArray *entries = g_ptr_array_new_with_free_func (entry_free);
  while (i < n)
    {
      gsize len = data[i], name_len;
      if (len == 0)
        {
          i = (i / SECTOR + 1) * SECTOR; /* records don't cross sectors */
          continue;
        }
      if (i + len > n || len < 34)
        break;
      name_len = data[i + 32];
      if (33 + name_len <= len)
        {
          g_autofree char *raw = g_strndup ((const char *) data + i + 33, name_len);
          char *semicolon = strchr (raw, ';');
          if (semicolon)
            *semicolon = '\0';
          if (name_len > 0 && !(name_len == 1 && (data[i + 33] == 0 || data[i + 33] == 1)))
            {
              Entry *e = g_new0 (Entry, 1);
              e->name = g_steal_pointer (&raw);
              e->lba = u32 (data, n, i + 2);
              e->size = u32 (data, n, i + 10);
              g_ptr_array_add (entries, e);
            }
        }
      i += len;
    }
  return entries;
}

static gint64 *
boxed (gint64 value)
{
  gint64 *v = g_new (gint64, 1);
  *v = value;
  return v;
}

static GBytes *
source_read (BroByteSource *base, const char *path, gint64 offset, gsize length)
{
  BroVideoTsByteSource *self = (BroVideoTsByteSource *) base;
  const gint64 *size = g_hash_table_lookup (self->sizes, path);
  gsize count;
  if (!size || offset < 0 || offset >= *size || length == 0)
    return g_bytes_new (NULL, 0);
  count = (gsize) MIN ((gint64) length, *size - offset);
  if (self->image >= 0)
    return read_sectors (self->image, *(const gint64 *) g_hash_table_lookup (self->lba, path) * SECTOR + offset, count);
  {
    int fd = g_open (g_hash_table_lookup (self->paths, path), O_RDONLY | O_CLOEXEC, 0);
    GBytes *data;
    if (fd < 0)
      return g_bytes_new (NULL, 0);
    data = read_at (fd, offset, count);
    close (fd);
    return data;
  }
}

static int
by_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroByteFile *const *) a)->name, (*(BroByteFile *const *) b)->name);
}

static GPtrArray *
source_files (BroByteSource *base)
{
  BroVideoTsByteSource *self = (BroVideoTsByteSource *) base;
  GPtrArray *files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_byte_file_free);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, self->sizes);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_ptr_array_add (files, bro_byte_file_new (k, *(gint64 *) v));
  g_ptr_array_sort (files, by_name);
  return files;
}

static BroVideoTsByteSource *
source_new (int image, const char *label)
{
  BroVideoTsByteSource *self = g_new0 (BroVideoTsByteSource, 1);
  self->base.read = source_read;
  self->base.files = source_files;
  self->image = image;
  self->lba = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->sizes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->label = g_strdup (label);
  return self;
}

static BroVideoTsByteSource *
open_folder (const char *path)
{
  g_autofree char *dir = g_strdup (path), *base = NULL, *parent = NULL, *parent_name = NULL;
  BroVideoTsByteSource *self;
  GDir *d;
  const char *name;
  gsize n = strlen (dir);
  while (n > 1 && dir[n - 1] == '/')
    dir[--n] = '\0';
  base = g_path_get_basename (dir);
  if (g_ascii_strcasecmp (base, "VIDEO_TS") != 0)
    {
      char *found = NULL;
      d = g_dir_open (dir, 0, NULL);
      while (d && !found && (name = g_dir_read_name (d)) != NULL)
        if (g_ascii_strcasecmp (name, "VIDEO_TS") == 0)
          found = g_build_filename (dir, name, NULL);
      if (d)
        g_dir_close (d);
      if (!found || !g_file_test (found, G_FILE_TEST_IS_DIR))
        {
          g_free (found);
          return NULL;
        }
      g_free (dir);
      dir = found;
    }
  parent = g_path_get_dirname (dir);
  parent_name = g_path_get_basename (parent);
  self = source_new (-1, parent_name);
  d = g_dir_open (dir, 0, NULL);
  while (d && (name = g_dir_read_name (d)) != NULL)
    {
      g_autofree char *file = g_build_filename (dir, name, NULL);
      GStatBuf st;
      if (g_stat (file, &st) != 0 || !S_ISREG (st.st_mode))
        continue;
      g_hash_table_insert (self->paths, g_ascii_strup (name, -1), g_steal_pointer (&file));
      g_hash_table_insert (self->sizes, g_ascii_strup (name, -1), boxed (st.st_size));
    }
  if (d)
    g_dir_close (d);
  if (!g_hash_table_contains (self->sizes, "VIDEO_TS.IFO"))
    {
      bro_video_ts_byte_source_free (self);
      return NULL;
    }
  return self;
}

static BroVideoTsByteSource *
open_image (const char *path)
{
  int fd = g_open (path, O_RDONLY | O_CLOEXEC, 0);
  g_autoptr (GBytes) pvd_bytes = NULL;
  g_autoptr (GPtrArray) root_entries = NULL, entries = NULL;
  g_autofree char *label = NULL;
  BroVideoTsByteSource *self;
  const guint8 *pvd;
  gsize n = 0;
  Entry *video_ts = NULL;
  if (fd < 0)
    return NULL;
  pvd_bytes = read_sectors (fd, 16 * SECTOR, SECTOR);
  pvd = g_bytes_get_data (pvd_bytes, &n);
  if (n < 190 || memcmp (pvd + 1, "CD001", 5) != 0)
    {
      close (fd);
      return NULL;
    }
  label = g_strndup ((const char *) pvd + 40, 32);
  g_strchomp (label);
  self = source_new (fd, label);
  root_entries = directory (fd, u32 (pvd, n, 156 + 2), u32 (pvd, n, 156 + 10));
  for (guint i = 0; i < root_entries->len && !video_ts; i++)
    if (g_ascii_strcasecmp (((Entry *) root_entries->pdata[i])->name, "VIDEO_TS") == 0)
      video_ts = root_entries->pdata[i];
  if (!video_ts)
    {
      bro_video_ts_byte_source_free (self);
      return NULL;
    }
  entries = directory (fd, video_ts->lba, video_ts->size);
  for (guint i = 0; i < entries->len; i++)
    {
      Entry *e = entries->pdata[i];
      g_hash_table_insert (self->lba, g_ascii_strup (e->name, -1), boxed (e->lba));
      g_hash_table_insert (self->sizes, g_ascii_strup (e->name, -1), boxed (e->size));
    }
  return self;
}

BroVideoTsByteSource *
bro_video_ts_byte_source_open (const char *path)
{
  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    return open_folder (path);
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    return open_image (path);
  return NULL;
}

const char *
bro_video_ts_byte_source_label (BroVideoTsByteSource *self)
{
  return self->label;
}

void
bro_video_ts_byte_source_free (BroVideoTsByteSource *self)
{
  if (!self)
    return;
  if (self->image >= 0)
    close (self->image);
  g_hash_table_unref (self->lba);
  g_hash_table_unref (self->paths);
  g_hash_table_unref (self->sizes);
  g_free (self->label);
  g_free (self);
}
