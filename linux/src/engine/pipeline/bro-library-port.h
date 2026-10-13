/* bro-library-port.h: BroLibraryPort: What a step may do with a library (plan §12.2), implemented by LibraryService. */
#pragma once

#include "bro-bro-error.h"
#include "bro-commit-intent.h"
#include "bro-commit-result.h"
#include "bro-id.h"
#include "bro-library-handle.h"
#include "bro-unit-file.h"
#include "bro-unit-record.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_LIBRARY_PORT (bro_library_port_get_type ())
G_DECLARE_INTERFACE (BroLibraryPort, bro_library_port, BRO, LIBRARY_PORT, GObject)

struct _BroLibraryPortInterface {
  GTypeInterface parent_iface;

  BroLibraryHandle *(*require) (BroLibraryPort *self, const char *library_id, BroBroError **error);
  char *(*staging_directory) (BroLibraryPort *self, const char *library_id, const BroId *job_id, BroBroError **error);
  BroCommitResult *(*commit) (BroLibraryPort *self, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files,
                              BroBroError **error);
  BroCommitResult *(*quarantine) (BroLibraryPort *self, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files,
                                  const char *tag, BroBroError **error);
};

/* library.notFound, or the reason it is offline (library.offline when its marker is missing). */
BroLibraryHandle *bro_library_port_require (BroLibraryPort *self, const char *library_id, BroBroError **error);

/* Where the job's files are made: <library>/.bromelia/staging/<job> (the same file system, so the commit is a rename),
 * made when it isn't there; never the library's root. */
char *bro_library_port_staging_directory (BroLibraryPort *self, const char *library_id, const BroId *job_id, BroBroError **error);

/* Plan §22.3 steps 4–6 (Seal did 1–3): the intent, the move (a new folder renamed whole; a shared one item by item,
 * SHA256SUMS merged), the folders synced, then the unit committed with its files (BroUnitFile *, copied). On a worker,
 * never the executor. NULL and @error set on failure. */
BroCommitResult *bro_library_port_commit (BroLibraryPort *self, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files,
                                          BroBroError **error);

/* bro_library_port_commit into <library>/_Quarantine/<name> [<tag>] (a free name), the unit quarantined. */
BroCommitResult *bro_library_port_quarantine (BroLibraryPort *self, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files,
                                              const char *tag, BroBroError **error);

G_END_DECLS
