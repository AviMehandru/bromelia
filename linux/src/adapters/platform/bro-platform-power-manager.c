/* bro-platform-power-manager.c */
#include "bro-platform-power-manager.h"

#include "bro-message-code.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

/* ---- the guard: the inhibitor's file descriptor ---- */

#define BRO_TYPE_LOGIND_GUARD (bro_logind_guard_get_type ())
G_DECLARE_FINAL_TYPE (BroLogindGuard, bro_logind_guard, BRO, LOGIND_GUARD, GObject)

struct _BroLogindGuard {
  GObject parent_instance;
  GMutex lock;
  int fd;
};

static void bro_logind_guard_iface_init (BroPowerGuardInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroLogindGuard, bro_logind_guard, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_POWER_GUARD, bro_logind_guard_iface_init))

static void
guard_release (BroPowerGuard *guard)
{
  BroLogindGuard *self = BRO_LOGIND_GUARD (guard);
  g_mutex_lock (&self->lock);
  if (self->fd >= 0)
    close (self->fd);
  self->fd = -1;
  g_mutex_unlock (&self->lock);
}

static void
bro_logind_guard_finalize (GObject *object)
{
  guard_release (BRO_POWER_GUARD (object));
  g_mutex_clear (&BRO_LOGIND_GUARD (object)->lock);
  G_OBJECT_CLASS (bro_logind_guard_parent_class)->finalize (object);
}

static void bro_logind_guard_class_init (BroLogindGuardClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_logind_guard_finalize; }

static void
bro_logind_guard_init (BroLogindGuard *self)
{
  g_mutex_init (&self->lock);
  self->fd = -1;
}

static void bro_logind_guard_iface_init (BroPowerGuardInterface *iface) { iface->release = guard_release; }

/* ---- the manager ---- */

struct _BroPlatformPowerManager {
  GObject parent_instance;
};

static void bro_platform_power_manager_iface_init (BroPowerManagerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformPowerManager, bro_platform_power_manager, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_POWER_MANAGER, bro_platform_power_manager_iface_init))

static gboolean
unavailable (BroBroError **error, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_POWER_UNAVAILABLE), params);
  return FALSE;
}

/* logind's Inhibit; the file descriptor, or -1 with @gerror set. */
static int
inhibit (GDBusConnection *bus, const char *what, const char *reason, GError **gerror)
{
  g_autoptr (GUnixFDList) fds = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_with_unix_fd_list_sync (bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                                                             "org.freedesktop.login1.Manager", "Inhibit",
                                                                             g_variant_new ("(ssss)", what, "Bromelia", reason, "block"),
                                                                             G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &fds,
                                                                             NULL, gerror);
  if (!reply)
    return -1;
  if (!fds || g_unix_fd_list_get_length (fds) < 1)
    {
      g_set_error_literal (gerror, G_IO_ERROR, G_IO_ERROR_FAILED, "no file descriptor in logind's answer");
      return -1;
    }
  return g_unix_fd_list_get (fds, 0, gerror);
}

BroPowerGuard *
bro_platform_power_manager_inhibit (BroPlatformPowerManager *self, const char *reason, BroBroError **error)
{
  g_autoptr (GError) gerror = NULL;
  g_autofree char *address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SYSTEM, NULL, &gerror);
  g_autoptr (GDBusConnection) bus = NULL;
  BroLogindGuard *guard;
  int fd;
  /* A connection of its own (not the shared system bus): the fd outlives it, and the address is read every time. */
  if (address)
    bus = g_dbus_connection_new_for_address_sync (address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                  NULL, NULL, &gerror);
  if (!bus)
    {
      unavailable (error, gerror ? gerror->message : "no system bus");
      return NULL;
    }
  fd = inhibit (bus, "sleep:idle", reason, &gerror);
  if (fd < 0 && (g_dbus_error_is_remote_error (gerror) || g_error_matches (gerror, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)))
    {
      g_autofree char *name = g_dbus_error_get_remote_error (gerror);
      if (name && (g_str_has_suffix (name, "AccessDenied") || g_str_has_suffix (name, "InteractiveAuthorizationRequired")))
        {
          g_clear_error (&gerror);
          fd = inhibit (bus, "idle", reason, &gerror);
        }
    }
  g_dbus_connection_close_sync (bus, NULL, NULL);
  if (fd < 0)
    {
      unavailable (error, gerror ? gerror->message : "logind refused");
      return NULL;
    }
  guard = g_object_new (BRO_TYPE_LOGIND_GUARD, NULL);
  guard->fd = fd;
  return BRO_POWER_GUARD (guard);
}

static BroPowerGuard *
inhibit_vfunc (BroPowerManager *self, const char *reason, BroBroError **error)
{
  return bro_platform_power_manager_inhibit (BRO_PLATFORM_POWER_MANAGER (self), reason, error);
}

static void bro_platform_power_manager_class_init (BroPlatformPowerManagerClass *klass) {}
static void bro_platform_power_manager_init (BroPlatformPowerManager *self) {}
static void bro_platform_power_manager_iface_init (BroPowerManagerInterface *iface) { iface->inhibit = inhibit_vfunc; }

BroPlatformPowerManager *
bro_platform_power_manager_new (void)
{
  return g_object_new (BRO_TYPE_PLATFORM_POWER_MANAGER, NULL);
}
