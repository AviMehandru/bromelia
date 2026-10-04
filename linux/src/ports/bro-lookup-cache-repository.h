/* bro-lookup-cache-repository.h: BroLookupCacheRepository: Online lookup answers (lookup_cache). */
#pragma once

#include "bro-bro-error.h"
#include "bro-http-response.h"
#include "bro-instant.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_LOOKUP_CACHE_REPOSITORY (bro_lookup_cache_repository_get_type ())
G_DECLARE_INTERFACE (BroLookupCacheRepository, bro_lookup_cache_repository, BRO, LOOKUP_CACHE_REPOSITORY, GObject)

struct _BroLookupCacheRepositoryInterface {
  GTypeInterface parent_iface;

  BroHttpResponse *(*get) (BroLookupCacheRepository *self, const char *provider, const char *key, BroBroError **error);
  gboolean (*put) (BroLookupCacheRepository *self, const char *provider, const char *key, const BroHttpResponse *response, BroInstant expires_at, BroBroError **error);
};

/* The answer, while it hasn't expired. NULL when there is none. */
BroHttpResponse *bro_lookup_cache_repository_get (BroLookupCacheRepository *self, const char *provider, const char *key, BroBroError **error);

/* key: the request without credentials. FALSE and @error set on failure. */
gboolean bro_lookup_cache_repository_put (BroLookupCacheRepository *self, const char *provider, const char *key, const BroHttpResponse *response, BroInstant expires_at, BroBroError **error);

G_END_DECLS
