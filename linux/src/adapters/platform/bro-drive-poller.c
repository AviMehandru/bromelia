/* bro-drive-poller.c */
#include "bro-drive-poller.h"

struct _BroDrivePoller {
  GObject parent_instance;
  BroOsDriveSnapshotFunc snapshot;
  gpointer data;
  GDestroyNotify destroy;
  BroClock *clock;
  BroDuration interval;
  GMutex lock;
  GPtrArray *last;
  BroTimerHandle *timer;
  BroDeviceSinkFunc sink;
  gpointer sink_data;
  GDestroyNotify sink_destroy;
};

static void bro_drive_poller_iface_init (BroDeviceMonitorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroDrivePoller, bro_drive_poller, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DEVICE_MONITOR, bro_drive_poller_iface_init))

/* One look; the events go to the sink outside the lock (the sink may stop the poller). */
static void
look (gpointer data)
{
  BroDrivePoller *self = data;
  g_autoptr (GPtrArray) events = NULL;
  BroDeviceSinkFunc sink;
  gpointer sink_data;
  g_mutex_lock (&self->lock);
  if (!self->sink)
    {
      g_mutex_unlock (&self->lock);
      return;
    }
  {
    GPtrArray *now = self->snapshot (self->data);
    events = bro_os_drive_state_changes (self->last, now);
    g_clear_pointer (&self->last, g_ptr_array_unref);
    self->last = now;
  }
  sink = self->sink;
  sink_data = self->sink_data;
  g_mutex_unlock (&self->lock);
  for (guint i = 0; i < events->len; i++)
    sink (events->pdata[i], sink_data);
}

void
bro_drive_poller_start (BroDrivePoller *self, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy)
{
  BroTimerSchedule every = { BRO_TIMER_SCHEDULE_EVERY, { 0 }, self->interval };
  g_mutex_lock (&self->lock);
  if (self->timer)
    {
      g_mutex_unlock (&self->lock);
      if (destroy)
        destroy (data);
      return;
    }
  self->sink = sink;
  self->sink_data = data;
  self->sink_destroy = destroy;
  g_clear_pointer (&self->last, g_ptr_array_unref);
  self->last = self->snapshot (self->data);
  g_mutex_unlock (&self->lock);
  /* The timer holds a reference until it is cancelled and done. */
  {
    BroTimerHandle *timer = bro_clock_timer (self->clock, &every, look, g_object_ref (self), g_object_unref);
    g_mutex_lock (&self->lock);
    self->timer = timer;
    g_mutex_unlock (&self->lock);
  }
}

void
bro_drive_poller_stop (BroDrivePoller *self)
{
  g_autoptr (BroTimerHandle) timer = NULL;
  gpointer sink_data;
  GDestroyNotify sink_destroy;
  g_mutex_lock (&self->lock);
  timer = g_steal_pointer (&self->timer);
  sink_data = self->sink_data;
  sink_destroy = self->sink_destroy;
  self->sink = NULL;
  self->sink_data = NULL;
  self->sink_destroy = NULL;
  g_mutex_unlock (&self->lock);
  if (timer)
    bro_timer_handle_cancel (timer);
  if (sink_destroy)
    sink_destroy (sink_data);
}

GPtrArray *
bro_drive_poller_current_drives (BroDrivePoller *self)
{
  g_autoptr (GPtrArray) states = self->snapshot (self->data);
  GPtrArray *drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
  for (guint i = 0; i < states->len; i++)
    g_ptr_array_add (drives, bro_os_drive_copy (((BroOsDriveState *) states->pdata[i])->drive));
  return drives;
}

static void start_vfunc (BroDeviceMonitor *m, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy) { bro_drive_poller_start (BRO_DRIVE_POLLER (m), sink, data, destroy); }
static void stop_vfunc (BroDeviceMonitor *m) { bro_drive_poller_stop (BRO_DRIVE_POLLER (m)); }
static GPtrArray *current_vfunc (BroDeviceMonitor *m) { return bro_drive_poller_current_drives (BRO_DRIVE_POLLER (m)); }

static void
bro_drive_poller_iface_init (BroDeviceMonitorInterface *iface)
{
  iface->start = start_vfunc;
  iface->stop = stop_vfunc;
  iface->current_drives = current_vfunc;
}

static void
bro_drive_poller_finalize (GObject *object)
{
  BroDrivePoller *self = BRO_DRIVE_POLLER (object);
  bro_drive_poller_stop (self);
  g_clear_pointer (&self->last, g_ptr_array_unref);
  if (self->destroy)
    self->destroy (self->data);
  g_clear_object (&self->clock);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_drive_poller_parent_class)->finalize (object);
}

static void bro_drive_poller_class_init (BroDrivePollerClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_drive_poller_finalize; }
static void bro_drive_poller_init (BroDrivePoller *self) { g_mutex_init (&self->lock); }

BroDrivePoller *
bro_drive_poller_new (BroOsDriveSnapshotFunc snapshot, gpointer data, GDestroyNotify destroy, BroClock *clock, BroDuration interval)
{
  BroDrivePoller *self = g_object_new (BRO_TYPE_DRIVE_POLLER, NULL);
  self->snapshot = snapshot;
  self->data = data;
  self->destroy = destroy;
  self->clock = g_object_ref (clock);
  self->interval = interval;
  return self;
}
