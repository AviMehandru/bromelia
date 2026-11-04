/* bro-no-drives.c */
#include "bro-no-drives.h"

#include "bro-message-code.h"
#include "bro-makemkv-drive.h"

/* ---- the device monitor ---- */

#define BRO_TYPE_NO_DEVICE_MONITOR (bro_no_device_monitor_get_type ())
G_DECLARE_FINAL_TYPE (BroNoDeviceMonitor, bro_no_device_monitor, BRO, NO_DEVICE_MONITOR, GObject)

struct _BroNoDeviceMonitor {
  GObject parent_instance;
};

static void
monitor_start (BroDeviceMonitor *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy)
{
  (void) self;
  (void) sink;
  if (destroy)
    destroy (data);
}

static void
monitor_stop (BroDeviceMonitor *self)
{
  (void) self;
}

static GPtrArray *
monitor_current_drives (BroDeviceMonitor *self)
{
  (void) self;
  return g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
}

static void
monitor_iface_init (BroDeviceMonitorInterface *iface)
{
  iface->start = monitor_start;
  iface->stop = monitor_stop;
  iface->current_drives = monitor_current_drives;
}

G_DEFINE_TYPE_WITH_CODE (BroNoDeviceMonitor, bro_no_device_monitor, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_DEVICE_MONITOR, monitor_iface_init))

static void
bro_no_device_monitor_class_init (BroNoDeviceMonitorClass *klass)
{
  (void) klass;
}

static void
bro_no_device_monitor_init (BroNoDeviceMonitor *self)
{
  (void) self;
}

BroDeviceMonitor *
bro_no_device_monitor_new (void)
{
  return BRO_DEVICE_MONITOR (g_object_new (BRO_TYPE_NO_DEVICE_MONITOR, NULL));
}

/* ---- the drive control ---- */

#define BRO_TYPE_NO_DRIVE_CONTROL (bro_no_drive_control_get_type ())
G_DECLARE_FINAL_TYPE (BroNoDriveControl, bro_no_drive_control, BRO, NO_DRIVE_CONTROL, GObject)

struct _BroNoDriveControl {
  GObject parent_instance;
};

static void
refuse (const char *device, BroBroError **error)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "driveId", bro_json_value_new_string (device));
  if (error)
    *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_DRIVE_NOT_FOUND), params, NULL);
  else
    bro_json_value_unref (params);
}

static gboolean
control_eject (BroDriveControl *self, const char *device, BroBroError **error)
{
  (void) self;
  refuse (device, error);
  return FALSE;
}

static gboolean
control_close_tray (BroDriveControl *self, const char *device, BroBroError **error)
{
  (void) self;
  refuse (device, error);
  return FALSE;
}

static char *
control_wait_for_mount (BroDriveControl *self, const char *device, BroDuration timeout, BroCancellationToken *cancel)
{
  (void) self;
  (void) device;
  (void) timeout;
  (void) cancel;
  return NULL;
}

static BroDiscContent
control_probe_content (BroDriveControl *self, const char *device)
{
  (void) self;
  (void) device;
  return BRO_DISC_CONTENT_UNKNOWN;
}

static BroSectorReader *
control_open_raw (BroDriveControl *self, const char *device, BroBroError **error)
{
  (void) self;
  refuse (device, error);
  return NULL;
}

static void
control_iface_init (BroDriveControlInterface *iface)
{
  iface->eject = control_eject;
  iface->close_tray = control_close_tray;
  iface->wait_for_mount = control_wait_for_mount;
  iface->probe_content = control_probe_content;
  iface->open_raw = control_open_raw;
}

G_DEFINE_TYPE_WITH_CODE (BroNoDriveControl, bro_no_drive_control, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, control_iface_init))

static void
bro_no_drive_control_class_init (BroNoDriveControlClass *klass)
{
  (void) klass;
}

static void
bro_no_drive_control_init (BroNoDriveControl *self)
{
  (void) self;
}

BroDriveControl *
bro_no_drive_control_new (void)
{
  return BRO_DRIVE_CONTROL (g_object_new (BRO_TYPE_NO_DRIVE_CONTROL, NULL));
}

/* ---- MakeMKV ---- */

#define BRO_TYPE_NO_DRIVE_SCAN (bro_no_drive_scan_get_type ())
G_DECLARE_FINAL_TYPE (BroNoDriveScan, bro_no_drive_scan, BRO, NO_DRIVE_SCAN, GObject)

struct _BroNoDriveScan {
  GObject parent_instance;
  BroMakemkv *makemkv;
};

static GPtrArray *
scan_scan_drives (BroMakemkv *self, BroCancellationToken *cancel, BroBroError **error)
{
  (void) self;
  (void) cancel;
  (void) error;
  return g_ptr_array_new_with_free_func ((GDestroyNotify) bro_makemkv_drive_free);
}

static BroListingRun *
scan_listing (BroMakemkv *self, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation, BroRunSink *sink,
              BroCancellationToken *cancel, BroBroError **error)
{
  return bro_makemkv_listing (((BroNoDriveScan *) self)->makemkv, source, invocation, sink, cancel, error);
}

static BroMakemkvRun *
scan_rip (BroMakemkv *self, const BroMakemkvSource *source, const char *title, const char *destination,
          const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  return bro_makemkv_rip (((BroNoDriveScan *) self)->makemkv, source, title, destination, invocation, sink, cancel, error);
}

static BroMakemkvRun *
scan_backup (BroMakemkv *self, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
             const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  return bro_makemkv_backup (((BroNoDriveScan *) self)->makemkv, source, decrypt, destination, invocation, sink, cancel, error);
}

static void
scan_iface_init (BroMakemkvInterface *iface)
{
  iface->scan_drives = scan_scan_drives;
  iface->listing = scan_listing;
  iface->rip = scan_rip;
  iface->backup = scan_backup;
}

G_DEFINE_TYPE_WITH_CODE (BroNoDriveScan, bro_no_drive_scan, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV, scan_iface_init))

static void
scan_finalize (GObject *object)
{
  g_clear_object (&((BroNoDriveScan *) object)->makemkv);
  G_OBJECT_CLASS (bro_no_drive_scan_parent_class)->finalize (object);
}

static void
bro_no_drive_scan_class_init (BroNoDriveScanClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = scan_finalize;
}

static void
bro_no_drive_scan_init (BroNoDriveScan *self)
{
  (void) self;
}

BroMakemkv *
bro_no_drive_scan_new (BroMakemkv *makemkv)
{
  BroNoDriveScan *self = g_object_new (BRO_TYPE_NO_DRIVE_SCAN, NULL);
  self->makemkv = g_object_ref (makemkv);
  return BRO_MAKEMKV (self);
}
