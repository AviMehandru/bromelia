/* bro-library-repository.h: BroLibraryRepository: libraries: the libraries LibraryService has seen, with their
 * markers. */
#pragma once

#include "bro-bro-error.h"
#include "bro-library-record.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_LIBRARY_REPOSITORY (bro_library_repository_get_type ())
G_DECLARE_INTERFACE (BroLibraryRepository, bro_library_repository, BRO, LIBRARY_REPOSITORY, GObject)

struct _BroLibraryRepositoryInterface {
  GTypeInterface parent_iface;

  GPtrArray *(*all) (BroLibraryRepository *self, BroBroError **error);
  BroLibraryRecord *(*get) (BroLibraryRepository *self, const char *library_id, BroBroError **error);
  gboolean (*upsert) (BroLibraryRepository *self, const BroLibraryRecord *library, BroBroError **error);
  gboolean (*set_seen) (BroLibraryRepository *self, const char *library_id, gboolean online, BroInstant at, BroBroError **error);
};

/* By id: BroLibraryRecord. NULL and @error set on failure. */
GPtrArray *bro_library_repository_all (BroLibraryRepository *self, BroBroError **error);

/* NULL when there is no such row (or with @error set on failure). */
BroLibraryRecord *bro_library_repository_get (BroLibraryRepository *self, const char *library_id, BroBroError **error);

/* Inserts the row or replaces every column. FALSE and @error set on failure. */
gboolean bro_library_repository_upsert (BroLibraryRepository *self, const BroLibraryRecord *library, BroBroError **error);

/* Sets online; last_seen_at becomes @at only when online. Nothing when there is no such row. FALSE and @error set on
 * failure. */
gboolean bro_library_repository_set_seen (BroLibraryRepository *self, const char *library_id, gboolean online, BroInstant at, BroBroError **error);

G_END_DECLS
