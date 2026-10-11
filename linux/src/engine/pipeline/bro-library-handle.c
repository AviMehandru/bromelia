/* bro-library-handle.c */
#include "bro-library-handle.h"

BroLibraryHandle *
bro_library_handle_new (const char *id, const char *path)
{
  BroLibraryHandle *h = g_new0 (BroLibraryHandle, 1);
  h->id = g_strdup (id);
  h->path = g_strdup (path);
  return h;
}

void
bro_library_handle_free (BroLibraryHandle *handle)
{
  if (!handle)
    return;
  g_free (handle->id);
  g_free (handle->path);
  g_free (handle);
}
