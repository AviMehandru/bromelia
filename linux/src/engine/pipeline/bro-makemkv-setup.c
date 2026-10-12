/* bro-makemkv-setup.c */
#include "bro-makemkv-setup.h"

G_DEFINE_INTERFACE (BroMakemkvSetup, bro_makemkv_setup, G_TYPE_OBJECT)

static void
bro_makemkv_setup_default_init (BroMakemkvSetupInterface *iface)
{
}

BroMakemkvInvocation *
bro_makemkv_setup_invocation (BroMakemkvSetup *self, const BroEffectiveProfile *profile, const char *selection_override,
                              const char *job_directory, BroBroError **error)
{
  return BRO_MAKEMKV_SETUP_GET_IFACE (self)->invocation (self, profile, selection_override, job_directory, error);
}
