/* bro-library-repository.c */
#include "bro-library-repository.h"

G_DEFINE_INTERFACE (BroLibraryRepository, bro_library_repository, G_TYPE_OBJECT)

static void
bro_library_repository_default_init (BroLibraryRepositoryInterface *iface)
{
}

GPtrArray *
bro_library_repository_all (BroLibraryRepository *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_LIBRARY_REPOSITORY (self), NULL);
  return BRO_LIBRARY_REPOSITORY_GET_IFACE (self)->all (self, error);
}

BroLibraryRecord *
bro_library_repository_get (BroLibraryRepository *self, const char *library_id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_LIBRARY_REPOSITORY (self), NULL);
  return BRO_LIBRARY_REPOSITORY_GET_IFACE (self)->get (self, library_id, error);
}

gboolean
bro_library_repository_upsert (BroLibraryRepository *self, const BroLibraryRecord *library, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_LIBRARY_REPOSITORY (self), FALSE);
  return BRO_LIBRARY_REPOSITORY_GET_IFACE (self)->upsert (self, library, error);
}

gboolean
bro_library_repository_set_seen (BroLibraryRepository *self, const char *library_id, gboolean online, BroInstant at, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_LIBRARY_REPOSITORY (self), FALSE);
  return BRO_LIBRARY_REPOSITORY_GET_IFACE (self)->set_seen (self, library_id, online, at, error);
}
