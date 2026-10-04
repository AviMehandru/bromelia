/* bro-service-installer.h: BroServiceInstaller: bromeliad as a login item / service. */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SERVICE_INSTALLER (bro_service_installer_get_type ())
G_DECLARE_INTERFACE (BroServiceInstaller, bro_service_installer, BRO, SERVICE_INSTALLER, GObject)

struct _BroServiceInstallerInterface {
  GTypeInterface parent_iface;

  gboolean (*install) (BroServiceInstaller *self, BroBroError **error);
  gboolean (*uninstall) (BroServiceInstaller *self, BroBroError **error);
  gboolean (*is_installed) (BroServiceInstaller *self);
  gboolean (*start) (BroServiceInstaller *self, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_service_installer_install (BroServiceInstaller *self, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_service_installer_uninstall (BroServiceInstaller *self, BroBroError **error);

gboolean bro_service_installer_is_installed (BroServiceInstaller *self);

/* FALSE and @error set on failure. */
gboolean bro_service_installer_start (BroServiceInstaller *self, BroBroError **error);

G_END_DECLS
