/* bro-process-launcher.c */
#include "bro-process-launcher.h"

G_DEFINE_INTERFACE (BroProcessLauncher, bro_process_launcher, G_TYPE_OBJECT)

static void
bro_process_launcher_default_init (BroProcessLauncherInterface *iface)
{
}

BroRunningProcess *
bro_process_launcher_start (BroProcessLauncher *self, const BroProcessSpec *spec, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_PROCESS_LAUNCHER (self), NULL);
  return BRO_PROCESS_LAUNCHER_GET_IFACE (self)->start (self, spec, error);
}
