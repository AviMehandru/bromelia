/* bro-library-port.c */
#include "bro-library-port.h"

G_DEFINE_INTERFACE (BroLibraryPort, bro_library_port, G_TYPE_OBJECT)

static void
bro_library_port_default_init (BroLibraryPortInterface *iface)
{
}

BroLibraryHandle *
bro_library_port_require (BroLibraryPort *self, const char *library_id, BroBroError **error)
{
  return BRO_LIBRARY_PORT_GET_IFACE (self)->require (self, library_id, error);
}

char *
bro_library_port_staging_directory (BroLibraryPort *self, const char *library_id, const BroId *job_id, BroBroError **error)
{
  return BRO_LIBRARY_PORT_GET_IFACE (self)->staging_directory (self, library_id, job_id, error);
}
