/* bro-platform-drive-control.c */
#include "bro-platform-drive-control.h"

#include "bro-message-code.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#endif

#define SECTOR 2048

struct _BroPlatformDriveControl {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroClock *clock;
};

static void bro_platform_drive_control_iface_init (BroDriveControlInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformDriveControl, bro_platform_drive_control, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, bro_platform_drive_control_iface_init))

static gboolean
is_drive (const char *device)
{
  return device && g_str_has_prefix (device, "/dev/");
}

static void
fs_failed (BroBroError **error, const char *operation, const char *path, int code)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string (operation));
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string (g_strerror (code)));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_FAILED), params);
}

/* The eject tool's exit status; -1 when it can't start. */
static int
run_eject (BroPlatformDriveControl *self, const char *const *arguments)
{
  g_autoptr (BroProcessSpec) spec = bro_process_spec_new ();
  g_autoptr (BroRunningProcess) process = NULL;
  BroOutputLine *line;
  spec->executable = g_strdup ("eject");
  g_strfreev (spec->arguments);
  spec->arguments = g_strdupv ((char **) arguments);
  spec->stop_policy = BRO_STOP_POLICY_TERMINATE_FIRST;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = 60;
  process = bro_process_launcher_start (self->launcher, spec, NULL);
  if (!process)
    return -1;
  while ((line = bro_running_process_lines (process)) != NULL)
    bro_output_line_free (line);
  return bro_running_process_wait (process).status;
}

gboolean
bro_platform_drive_control_eject (BroPlatformDriveControl *self, const char *device, BroBroError **error)
{
  const char *args[] = { device, NULL };
  BroJsonValue *params;
  if (is_drive (device) && run_eject (self, args) == 0)
    return TRUE;
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "device", bro_json_value_new_string (device));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_EJECT_FAILED), params);
  return FALSE;
}

gboolean
bro_platform_drive_control_close_tray (BroPlatformDriveControl *self, const char *device, BroBroError **error)
{
  const char *args[] = { "-t", device, NULL };
  BroJsonValue *params;
  if (is_drive (device) && run_eject (self, args) == 0)
    return TRUE;
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "drive", bro_json_value_new_string (device));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_CLOSE_TRAY_FAILED), params);
  return FALSE;
}

/* Where @device (or what it links to, as /dev/cdrom does) is mounted, from /proc/self/mountinfo; NULL when it isn't. */
static char *
mount_path (const char *device)
{
  g_autofree char *text = NULL, *real = g_canonicalize_filename (device, NULL);
  g_auto (GStrv) lines = NULL;
  char resolved[4096];
  ssize_t n = readlink (device, resolved, sizeof resolved - 1);
  if (n > 0)
    {
      g_autofree char *dir = g_path_get_dirname (device);
      resolved[n] = '\0';
      g_free (real);
      real = g_canonicalize_filename (resolved, dir);
    }
  if (!g_file_get_contents ("/proc/self/mountinfo", &text, NULL, NULL))
    return NULL;
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_auto (GStrv) f = g_strsplit (lines[i], " ", -1);
      guint count = g_strv_length (f), dash = 0;
      for (guint k = 6; k < count && !dash; k++)
        if (g_str_equal (f[k], "-"))
          dash = k;
      if (dash && dash + 2 < count && (g_str_equal (f[dash + 2], device) || g_str_equal (f[dash + 2], real)))
        {
          GString *path = g_string_new (f[4]);
          g_string_replace (path, "\\040", " ", 0);
          return g_string_free (path, FALSE);
        }
    }
  return NULL;
}

char *
bro_platform_drive_control_wait_for_mount (BroPlatformDriveControl *self, const char *device, BroDuration timeout, BroCancellationToken *cancel)
{
  double deadline;
  if (!is_drive (device))
    return NULL;
  deadline = bro_clock_monotonic (self->clock).seconds + timeout.seconds;
  while (!(cancel && bro_cancellation_token_is_cancelled (cancel)))
    {
      char *path = mount_path (device);
      g_autoptr (BroBroError) error = NULL;
      if (path)
        return path;
      if (bro_clock_monotonic (self->clock).seconds >= deadline || !bro_clock_sleep (self->clock, (BroDuration) { 0.5 }, cancel, &error))
        return NULL;
    }
  return NULL;
}

static gboolean
has_video_structure (const char *root)
{
  GDir *d = g_dir_open (root, 0, NULL);
  const char *name;
  gboolean found = FALSE;
  while (d && !found && (name = g_dir_read_name (d)) != NULL)
    found = g_ascii_strcasecmp (name, "BDMV") == 0 || g_ascii_strcasecmp (name, "VIDEO_TS") == 0 || g_ascii_strcasecmp (name, "HVDVD_TS") == 0;
  if (d)
    g_dir_close (d);
  return found;
}

/* udev's properties of a block device (its database, /run/udev/data/b<major>:<minor>); NULL when there are none. */
static GHashTable *
udev_properties (const char *device)
{
#ifdef __linux__
  struct stat st;
  g_autofree char *path = NULL, *text = NULL;
  g_auto (GStrv) lines = NULL;
  GHashTable *props;
  if (stat (device, &st) != 0 || !S_ISBLK (st.st_mode))
    return NULL;
  path = g_strdup_printf ("/run/udev/data/b%u:%u", major (st.st_rdev), minor (st.st_rdev));
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return NULL;
  props = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      char *eq = strchr (lines[i], '=');
      if (g_str_has_prefix (lines[i], "E:") && eq)
        g_hash_table_replace (props, g_strndup (lines[i] + 2, eq - lines[i] - 2), g_strdup (eq + 1));
    }
  return props;
#else
  return NULL;
#endif
}

static int
prop_int (GHashTable *props, const char *key)
{
  const char *v = g_hash_table_lookup (props, key);
  return v ? atoi (v) : 0;
}

BroDiscContent
bro_platform_drive_control_probe_content (BroPlatformDriveControl *self, const char *device)
{
  g_autofree char *mount = NULL;
  g_autoptr (GHashTable) props = NULL;
  if (!is_drive (device))
    return BRO_DISC_CONTENT_UNKNOWN;
  /* A DVD / Blu-ray structure on the mounted disc means a video disc, whatever MakeMKV's flags said. */
  mount = mount_path (device);
  if (mount && has_video_structure (mount))
    return BRO_DISC_CONTENT_VIDEO;
  props = udev_properties (device);
  if (!props || g_strcmp0 (g_hash_table_lookup (props, "ID_CDROM_MEDIA"), "1") != 0)
    return BRO_DISC_CONTENT_UNKNOWN;
  if (g_strcmp0 (g_hash_table_lookup (props, "ID_CDROM_MEDIA_STATE"), "blank") == 0)
    return BRO_DISC_CONTENT_BLANK;
  /* Audio tracks make an audio CD (enhanced CDs have a data session too). */
  if (prop_int (props, "ID_CDROM_MEDIA_TRACK_COUNT_AUDIO") > 0)
    return BRO_DISC_CONTENT_AUDIO;
  if (prop_int (props, "ID_CDROM_MEDIA_TRACK_COUNT_DATA") > 0)
    return BRO_DISC_CONTENT_DATA;
  return BRO_DISC_CONTENT_UNKNOWN;
}

/* ---- raw reads ---- */

#define BRO_TYPE_RAW_READER (bro_raw_reader_get_type ())
G_DECLARE_FINAL_TYPE (BroRawReader, bro_raw_reader, BRO, RAW_READER, GObject)

struct _BroRawReader {
  GObject parent_instance;
  GMutex lock;
  int fd;
  char *path;
  gint64 sectors;
};

static void bro_raw_reader_iface_init (BroSectorReaderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroRawReader, bro_raw_reader, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_SECTOR_READER, bro_raw_reader_iface_init))

static GBytes *
raw_read (BroSectorReader *r, gint64 sector, int count, BroBroError **error)
{
  BroRawReader *self = BRO_RAW_READER (r);
  gsize n = (gsize) MAX (0, MIN ((gint64) count, self->sectors - sector)) * SECTOR, total = 0;
  guint8 *buffer = g_malloc (n ? n : 1);
  int failure = 0;
  g_mutex_lock (&self->lock);
  while (total < n && !failure)
    {
      ssize_t got = self->fd >= 0 ? pread (self->fd, buffer + total, n - total, (off_t) (sector * SECTOR + (gint64) total)) : -1;
      if (got < 0 && errno == EINTR)
        continue;
      if (got < 0)
        failure = self->fd >= 0 ? errno : EBADF;
      else if (got == 0)
        break;
      else
        total += (gsize) got;
    }
  g_mutex_unlock (&self->lock);
  if (failure)
    {
      g_free (buffer);
      fs_failed (error, "read", self->path, failure);
      return NULL;
    }
  return g_bytes_new_take (buffer, total);
}

static gint64 raw_count (BroSectorReader *r, BroBroError **error) { return BRO_RAW_READER (r)->sectors; }

static void
raw_close (BroSectorReader *r)
{
  BroRawReader *self = BRO_RAW_READER (r);
  g_mutex_lock (&self->lock);
  if (self->fd >= 0)
    close (self->fd);
  self->fd = -1;
  g_mutex_unlock (&self->lock);
}

static void
bro_raw_reader_finalize (GObject *o)
{
  BroRawReader *self = BRO_RAW_READER (o);
  raw_close (BRO_SECTOR_READER (o));
  g_free (self->path);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_raw_reader_parent_class)->finalize (o);
}

static void bro_raw_reader_class_init (BroRawReaderClass *k) { G_OBJECT_CLASS (k)->finalize = bro_raw_reader_finalize; }
static void bro_raw_reader_init (BroRawReader *self) { g_mutex_init (&self->lock); self->fd = -1; }

static void
bro_raw_reader_iface_init (BroSectorReaderInterface *iface)
{
  iface->read = raw_read;
  iface->sector_count = raw_count;
  iface->close = raw_close;
}

BroSectorReader *
bro_platform_drive_control_open_raw (BroPlatformDriveControl *self, const char *device, BroBroError **error)
{
  BroRawReader *reader;
  struct stat st;
  gint64 bytes = 0;
  int fd = g_open (device, O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    {
      int code = errno;
      if (code == ENOENT)
        {
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "path", bro_json_value_new_string (device));
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_NOT_FOUND), params);
        }
      else
        fs_failed (error, "open", device, code);
      return NULL;
    }
  if (fstat (fd, &st) == 0 && S_ISREG (st.st_mode))
    bytes = st.st_size;
  else
    {
      int code = ENOTSUP;
#ifdef __linux__
      guint64 size = 0;
      if (ioctl (fd, BLKGETSIZE64, &size) == 0)
        code = 0, bytes = (gint64) size;
      else
        code = errno;
#endif
      if (code)
        {
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "path", bro_json_value_new_string (device));
          bro_json_value_set (params, "reason", bro_json_value_new_string (g_strerror (code)));
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_SIZE_UNKNOWN), params);
          close (fd);
          return NULL;
        }
    }
  /* A partial last sector would be left out of every image. */
  if (bytes % SECTOR != 0)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (device));
      bro_json_value_set (params, "size", bro_json_value_new_integer (bytes));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_PARTIAL_SECTOR), params);
      close (fd);
      return NULL;
    }
  reader = g_object_new (BRO_TYPE_RAW_READER, NULL);
  reader->fd = fd;
  reader->path = g_strdup (device);
  reader->sectors = bytes / SECTOR;
  return BRO_SECTOR_READER (reader);
}

static gboolean eject_vfunc (BroDriveControl *c, const char *device, BroBroError **error) { return bro_platform_drive_control_eject (BRO_PLATFORM_DRIVE_CONTROL (c), device, error); }
static gboolean close_vfunc (BroDriveControl *c, const char *device, BroBroError **error) { return bro_platform_drive_control_close_tray (BRO_PLATFORM_DRIVE_CONTROL (c), device, error); }
static char *mount_vfunc (BroDriveControl *c, const char *device, BroDuration timeout, BroCancellationToken *cancel) { return bro_platform_drive_control_wait_for_mount (BRO_PLATFORM_DRIVE_CONTROL (c), device, timeout, cancel); }
static BroDiscContent probe_vfunc (BroDriveControl *c, const char *device) { return bro_platform_drive_control_probe_content (BRO_PLATFORM_DRIVE_CONTROL (c), device); }
static BroSectorReader *raw_vfunc (BroDriveControl *c, const char *device, BroBroError **error) { return bro_platform_drive_control_open_raw (BRO_PLATFORM_DRIVE_CONTROL (c), device, error); }

static void
bro_platform_drive_control_iface_init (BroDriveControlInterface *iface)
{
  iface->eject = eject_vfunc;
  iface->close_tray = close_vfunc;
  iface->wait_for_mount = mount_vfunc;
  iface->probe_content = probe_vfunc;
  iface->open_raw = raw_vfunc;
}

static void
bro_platform_drive_control_finalize (GObject *object)
{
  BroPlatformDriveControl *self = BRO_PLATFORM_DRIVE_CONTROL (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->clock);
  G_OBJECT_CLASS (bro_platform_drive_control_parent_class)->finalize (object);
}

static void bro_platform_drive_control_class_init (BroPlatformDriveControlClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_platform_drive_control_finalize; }
static void bro_platform_drive_control_init (BroPlatformDriveControl *self) {}

BroPlatformDriveControl *
bro_platform_drive_control_new (BroProcessLauncher *launcher, BroClock *clock)
{
  BroPlatformDriveControl *self = g_object_new (BRO_TYPE_PLATFORM_DRIVE_CONTROL, NULL);
  self->launcher = g_object_ref (launcher);
  self->clock = g_object_ref (clock);
  return self;
}
