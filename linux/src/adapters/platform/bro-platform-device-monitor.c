/* bro-platform-device-monitor.c */
#include "bro-platform-device-monitor.h"

#include "bro-drive-poller.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#endif

struct _BroPlatformDeviceMonitor {
  GObject parent_instance;
  BroDrivePoller *poller;
};

static void bro_platform_device_monitor_iface_init (BroDeviceMonitorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformDeviceMonitor, bro_platform_device_monitor, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DEVICE_MONITOR, bro_platform_device_monitor_iface_init))

static char *
sysfs_text (const char *name, const char *field)
{
  g_autofree char *path = g_build_filename ("/sys/class/block", name, "device", field, NULL);
  char *text = NULL;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return g_strdup ("");
  return g_strstrip (text);
}

/* /proc/self/mountinfo's octal escapes (\040 for a space). */
static char *
unescape (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; *p; p++)
    if (p[0] == '\\' && g_ascii_isdigit (p[1]) && g_ascii_isdigit (p[2]) && g_ascii_isdigit (p[3]))
      {
        g_string_append_c (out, (char) ((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0')));
        p += 3;
      }
    else
      g_string_append_c (out, *p);
  return g_string_free (out, FALSE);
}

/* Where @device is mounted, from /proc/self/mountinfo (the first mount); NULL when it isn't. */
static char *
mount_path (const char *device)
{
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  if (!g_file_get_contents ("/proc/self/mountinfo", &text, NULL, NULL))
    return NULL;
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_auto (GStrv) fields = g_strsplit (lines[i], " ", -1);
      guint n = g_strv_length (fields), dash = 0;
      for (guint k = 6; k < n && !dash; k++)
        if (g_str_equal (fields[k], "-"))
          dash = k;
      if (dash && dash + 2 < n && g_str_equal (fields[dash + 2], device))
        return unescape (fields[4]);
    }
  return NULL;
}

/* Whether the drive holds a disc; FALSE when it can't be asked. */
static gboolean
has_media (const char *device)
{
#ifdef __linux__
  int fd = open (device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  int status;
  if (fd < 0)
    return FALSE;
  status = ioctl (fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
  close (fd);
  return status == CDS_DISC_OK;
#else
  return FALSE;
#endif
}

static int
by_device (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroOsDriveState *const *) a)->drive->device, (*(BroOsDriveState *const *) b)->drive->device);
}

static GPtrArray *
snapshot_now (gpointer unused)
{
  GPtrArray *states = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_state_free);
  GDir *dir = g_dir_open ("/sys/class/block", 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *device = NULL, *vendor = NULL, *model = NULL, *identification = NULL, *mount = NULL;
      if (!g_str_has_prefix (name, "sr") || !g_ascii_isdigit (name[2]))
        continue;
      device = g_strconcat ("/dev/", name, NULL);
      vendor = sysfs_text (name, "vendor");
      model = sysfs_text (name, "model");
      identification = *vendor && *model ? g_strconcat (vendor, " ", model, NULL) : g_strconcat (vendor, model, NULL);
      mount = mount_path (device);
      g_ptr_array_add (states, bro_os_drive_state_new (bro_os_drive_new (device, identification, mount), has_media (device)));
    }
  if (dir)
    g_dir_close (dir);
  g_ptr_array_sort (states, by_device);
  return states;
}

GPtrArray *
bro_platform_device_monitor_snapshot (BroPlatformDeviceMonitor *self)
{
  return snapshot_now (NULL);
}

void
bro_platform_device_monitor_start (BroPlatformDeviceMonitor *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy)
{
  bro_drive_poller_start (self->poller, sink, data, destroy);
}

void
bro_platform_device_monitor_stop (BroPlatformDeviceMonitor *self)
{
  bro_drive_poller_stop (self->poller);
}

GPtrArray *
bro_platform_device_monitor_current_drives (BroPlatformDeviceMonitor *self)
{
  return bro_drive_poller_current_drives (self->poller);
}

static void start_vfunc (BroDeviceMonitor *m, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy) { bro_platform_device_monitor_start (BRO_PLATFORM_DEVICE_MONITOR (m), sink, data, destroy); }
static void stop_vfunc (BroDeviceMonitor *m) { bro_platform_device_monitor_stop (BRO_PLATFORM_DEVICE_MONITOR (m)); }
static GPtrArray *current_vfunc (BroDeviceMonitor *m) { return bro_platform_device_monitor_current_drives (BRO_PLATFORM_DEVICE_MONITOR (m)); }

static void
bro_platform_device_monitor_iface_init (BroDeviceMonitorInterface *iface)
{
  iface->start = start_vfunc;
  iface->stop = stop_vfunc;
  iface->current_drives = current_vfunc;
}

static void
bro_platform_device_monitor_finalize (GObject *object)
{
  BroPlatformDeviceMonitor *self = BRO_PLATFORM_DEVICE_MONITOR (object);
  bro_drive_poller_stop (self->poller);
  g_clear_object (&self->poller);
  G_OBJECT_CLASS (bro_platform_device_monitor_parent_class)->finalize (object);
}

static void bro_platform_device_monitor_class_init (BroPlatformDeviceMonitorClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_platform_device_monitor_finalize; }
static void bro_platform_device_monitor_init (BroPlatformDeviceMonitor *self) {}

BroPlatformDeviceMonitor *
bro_platform_device_monitor_new (BroClock *clock, double interval_seconds)
{
  BroPlatformDeviceMonitor *self = g_object_new (BRO_TYPE_PLATFORM_DEVICE_MONITOR, NULL);
  self->poller = bro_drive_poller_new (snapshot_now, NULL, NULL, clock, (BroDuration) { interval_seconds > 0 ? interval_seconds : 2 });
  return self;
}
