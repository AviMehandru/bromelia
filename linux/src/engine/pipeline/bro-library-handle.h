/* bro-library-handle.h: an online library: its id and its root folder. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *path;
} BroLibraryHandle;

BroLibraryHandle *bro_library_handle_new (const char *id, const char *path);
void bro_library_handle_free (BroLibraryHandle *handle);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLibraryHandle, bro_library_handle_free)

G_END_DECLS
