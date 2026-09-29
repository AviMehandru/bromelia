/* bro-sleep.c — a logind sleep inhibitor, for bromelia-daemon (the window app uses GTK's). */
#include "bro-sleep.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

typedef struct {
  BroSleepInhibitor base;
  GCancellable *cancellable; /* cancelled when freed, so late replies don't touch it */
  gboolean wanted, pending, reported;
  int fd;
} Logind;

static void request (Logind *self);

static void
report (Logind *self, const char *why)
{
  g_autofree char *msg = NULL;
  if (self->reported)
    return;
  self->reported = TRUE;
  msg = g_strdup_printf ("Can't keep the computer awake while jobs run (systemd-logind: %s)", why);
  if (self->base.problem)
    self->base.problem (msg, self->base.problem_data);
  else
    g_message ("%s", msg);
}

static void
inhibit_done (GObject *src, GAsyncResult *res, gpointer data)
{
  Logind *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GUnixFDList) fds = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (src), &fds, res, &error);
  int fd = -1;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  self->pending = FALSE;
  if (!reply)
    {
      report (self, error->message);
      return;
    }
  if (fds && g_unix_fd_list_get_length (fds) > 0)
    fd = g_unix_fd_list_get (fds, 0, NULL);
  if (fd < 0)
    {
      report (self, "no file descriptor in the reply");
      return;
    }
  if (self->wanted)
    self->fd = fd;
  else
    close (fd);
}

static void
bus_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  Logind *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) bus = g_bus_get_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!bus)
    {
      self->pending = FALSE;
      report (self, error->message);
      return;
    }
  if (!self->wanted)
    {
      self->pending = FALSE;
      return;
    }
  g_dbus_connection_call_with_unix_fd_list (bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                            "org.freedesktop.login1.Manager", "Inhibit",
                                            g_variant_new ("(ssss)", "sleep:idle", "Bromelia", "Ripping discs", "block"),
                                            G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, self->cancellable,
                                            inhibit_done, self);
}

static void
request (Logind *self)
{
  self->pending = TRUE;
  g_bus_get (G_BUS_TYPE_SYSTEM, self->cancellable, bus_ready, self);
}

static void
logind_set (BroSleepInhibitor *base, gboolean on)
{
  Logind *self = (Logind *) base;
  self->wanted = on;
  if (on && self->fd < 0 && !self->pending && !self->reported)
    request (self);
  else if (!on && self->fd >= 0)
    {
      close (self->fd);
      self->fd = -1;
    }
}

static void
logind_free (BroSleepInhibitor *base)
{
  Logind *self = (Logind *) base;
  g_cancellable_cancel (self->cancellable);
  g_object_unref (self->cancellable);
  if (self->fd >= 0)
    close (self->fd);
  g_free (self);
}

BroSleepInhibitor *
bro_sleep_inhibitor_logind_new (void)
{
  Logind *self = g_new0 (Logind, 1);
  self->base.set = logind_set;
  self->base.free = logind_free;
  self->cancellable = g_cancellable_new ();
  self->fd = -1;
  return &self->base;
}

void
bro_sleep_inhibitor_free (BroSleepInhibitor *self)
{
  if (self)
    self->free (self);
}
