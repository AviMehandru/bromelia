/* bro-makemkv.c */
#include "bro-makemkv.h"

G_DEFINE_INTERFACE (BroMakemkv, bro_makemkv, G_TYPE_OBJECT)

static void
bro_makemkv_default_init (BroMakemkvInterface *iface)
{
}

GPtrArray *
bro_makemkv_scan_drives (BroMakemkv *self, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MAKEMKV_GET_IFACE (self)->scan_drives (self, cancel, error);
}

BroListingRun *
bro_makemkv_listing (BroMakemkv *self, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation, BroRunSink *sink,
                     BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MAKEMKV_GET_IFACE (self)->listing (self, source, invocation, sink, cancel, error);
}

BroMakemkvRun *
bro_makemkv_rip (BroMakemkv *self, const BroMakemkvSource *source, const char *title, const char *destination,
                 const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MAKEMKV_GET_IFACE (self)->rip (self, source, title, destination, invocation, sink, cancel, error);
}

BroMakemkvRun *
bro_makemkv_backup (BroMakemkv *self, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
                    const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MAKEMKV_GET_IFACE (self)->backup (self, source, decrypt, destination, invocation, sink, cancel, error);
}
