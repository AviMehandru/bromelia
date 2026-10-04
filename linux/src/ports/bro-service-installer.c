/* bro-service-installer.c */
#include "bro-service-installer.h"

G_DEFINE_INTERFACE (BroServiceInstaller, bro_service_installer, G_TYPE_OBJECT)

static void
bro_service_installer_default_init (BroServiceInstallerInterface *iface)
{
}

gboolean
bro_service_installer_install (BroServiceInstaller *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_SERVICE_INSTALLER (self), FALSE);
  return BRO_SERVICE_INSTALLER_GET_IFACE (self)->install (self, error);
}

gboolean
bro_service_installer_uninstall (BroServiceInstaller *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_SERVICE_INSTALLER (self), FALSE);
  return BRO_SERVICE_INSTALLER_GET_IFACE (self)->uninstall (self, error);
}

gboolean
bro_service_installer_is_installed (BroServiceInstaller *self)
{
  g_return_val_if_fail (BRO_IS_SERVICE_INSTALLER (self), FALSE);
  return BRO_SERVICE_INSTALLER_GET_IFACE (self)->is_installed (self);
}

gboolean
bro_service_installer_start (BroServiceInstaller *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_SERVICE_INSTALLER (self), FALSE);
  return BRO_SERVICE_INSTALLER_GET_IFACE (self)->start (self, error);
}
