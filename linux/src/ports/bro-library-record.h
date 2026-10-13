/* bro-library-record.h: BroLibraryRecord: A row of libraries: the config entry's id, the id in its .bromelia-library
 * marker, whether the marker was there at the last check and when it was last seen. */
#pragma once

#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *id; /* the config entry's id */
  char *name;
  char *path;
  char *marker_id;
  BroInstant attached_at;
  gboolean online;
  gboolean has_last_seen_at;
  BroInstant last_seen_at;
} BroLibraryRecord;

/* Everything zero. */
BroLibraryRecord *bro_library_record_new (void);
BroLibraryRecord *bro_library_record_copy (const BroLibraryRecord *value);
void bro_library_record_free (BroLibraryRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLibraryRecord, bro_library_record_free)

G_END_DECLS
