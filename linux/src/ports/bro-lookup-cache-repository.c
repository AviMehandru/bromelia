/* bro-lookup-cache-repository.c */
#include "bro-lookup-cache-repository.h"

G_DEFINE_INTERFACE (BroLookupCacheRepository, bro_lookup_cache_repository, G_TYPE_OBJECT)

static void
bro_lookup_cache_repository_default_init (BroLookupCacheRepositoryInterface *iface)
{
}

BroHttpResponse *
bro_lookup_cache_repository_get (BroLookupCacheRepository *self, const char *provider, const char *key, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_LOOKUP_CACHE_REPOSITORY (self), NULL);
  return BRO_LOOKUP_CACHE_REPOSITORY_GET_IFACE (self)->get (self, provider, key, error);
}

gboolean
bro_lookup_cache_repository_put (BroLookupCacheRepository *self, const char *provider, const char *key, const BroHttpResponse *response, BroInstant expires_at, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_LOOKUP_CACHE_REPOSITORY (self), FALSE);
  return BRO_LOOKUP_CACHE_REPOSITORY_GET_IFACE (self)->put (self, provider, key, response, expires_at, error);
}
