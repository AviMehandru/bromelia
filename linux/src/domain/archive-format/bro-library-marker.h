/* bro-library-marker.h: BroLibraryMarker: the .bromelia-library file at a library's root (plan §22.2): written when a
 * library is attached (never on its own), so a library whose disk isn't there is offline instead of being written to
 * the boot disk. */
#pragma once

#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

#define BRO_LIBRARY_MARKER_FILE_NAME ".bromelia-library"

typedef struct {
  char *id;
  char *name;
  gboolean has_created;
  BroInstant created;
} BroLibraryMarker;

void bro_library_marker_free (BroLibraryMarker *marker);

/* A marker for a library attached now. Its id is a UUID (version 8, RFC 9562) made from the first 16 bytes of SHA-256
 * of "<library_id>\n<path>\n<created>": unique to this attach without any randomness. */
BroLibraryMarker *bro_library_marker_make (const char *library_id, const char *path, const char *name, BroInstant created);

/* {"id", "name", "created"}, canonical JSON with a final newline. */
GBytes *bro_library_marker_encode (const BroLibraryMarker *marker);

/* NULL unless a JSON object with a string id; the name defaults to "" and created to none. */
BroLibraryMarker *bro_library_marker_decode (GBytes *bytes);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLibraryMarker, bro_library_marker_free)

G_END_DECLS
