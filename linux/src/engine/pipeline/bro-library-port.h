/* bro-library-port.h: BroLibraryPort: What a step may do with a library (plan §12.2), implemented by LibraryService. The commit (§22.3) joins it with that service. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-library-handle.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_LIBRARY_PORT (bro_library_port_get_type ())
G_DECLARE_INTERFACE (BroLibraryPort, bro_library_port, BRO, LIBRARY_PORT, GObject)

struct _BroLibraryPortInterface {
  GTypeInterface parent_iface;
  BroLibraryHandle *(*require) (BroLibraryPort *self, const char *library_id, BroBroError **error);
  char *(*staging_directory) (BroLibraryPort *self, const char *library_id, const BroId *job_id, BroBroError **error);
};

/* library.offline when its marker is missing. */
BroLibraryHandle *bro_library_port_require (BroLibraryPort *self, const char *library_id, BroBroError **error);

/* Where the job's files are made: <library>/.bromelia/staging/<job> (the same file system, so the commit is a rename). */
char *bro_library_port_staging_directory (BroLibraryPort *self, const char *library_id, const BroId *job_id, BroBroError **error);

G_END_DECLS
