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

BroCommitResult *
bro_library_port_commit (BroLibraryPort *self, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files,
                         BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_LIBRARY_PORT_GET_IFACE (self)->commit (self, unit, intent, files, cancel, error);
}

BroCommitResult *
bro_library_port_quarantine (BroLibraryPort *self, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, const char *tag,
                             BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_LIBRARY_PORT_GET_IFACE (self)->quarantine (self, unit, intent, files, tag, cancel, error);
}
