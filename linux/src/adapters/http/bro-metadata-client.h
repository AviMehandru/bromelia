/* bro-metadata-client.h: BroMetadataClient: online lookup (plan §10.2; shared/fixtures/adapters/metadata-client.cases.json):
 * Domain's TMDb and OMDb requests and parsers over the HttpClient. A 200 answer is kept in the lookup cache for 7 days,
 * keyed by the provider and the request without the key. metadata.http for another status; http.failed is passed on;
 * metadata.idUnsupported for an id the provider can't take. Synchronous. */
#pragma once

#include "bro-candidate.h"
#include "bro-clock.h"
#include "bro-file-system.h"
#include "bro-http-client.h"
#include "bro-lookup-cache-repository.h"
#include "bro-online-id.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_METADATA_CLIENT (bro_metadata_client_get_type ())
G_DECLARE_FINAL_TYPE (BroMetadataClient, bro_metadata_client, BRO, METADATA_CLIENT, GObject)

/* @provider: "tmdb" or "omdb" (metadata.provider). @cache may be NULL (nothing cached). Keeps references. */
BroMetadataClient *bro_metadata_client_new (BroHttpClient *http, BroLookupCacheRepository *cache, BroClock *clock, BroFileSystem *fs,
                                            const char *provider, const char *key, const char *language);

/* BroCandidate *, ranked; a year that finds nothing is dropped (@year -1: none); OMDb's best result is read again for
 * its plot. NULL and @error set on failure. */
GPtrArray *bro_metadata_client_search (BroMetadataClient *self, const char *name, BroMediaKind kind, int year, BroCancellationToken *cancel,
                                       BroBroError **error);

/* NULL with @error unset when nothing was found. */
BroCandidate *bro_metadata_client_lookup (BroMetadataClient *self, const BroOnlineId *id, BroMediaKind kind, BroCancellationToken *cancel,
                                          BroBroError **error);

/* Episode number (GINT_TO_POINTER) → BroEpisodeDetails *. NULL and @error set on failure. */
GHashTable *bro_metadata_client_season (BroMetadataClient *self, const BroCandidate *match, int season, BroCancellationToken *cancel,
                                        BroBroError **error);

/* TMDb's absolute episode order (an episode group of type 2); empty when there is none or the provider is OMDb. */
GHashTable *bro_metadata_client_absolute_episodes (BroMetadataClient *self, const BroCandidate *match, BroCancellationToken *cancel,
                                                   BroBroError **error);

/* Downloads a poster to @destination (written atomically, not cached). */
gboolean bro_metadata_client_poster (BroMetadataClient *self, const char *url, const char *destination, BroCancellationToken *cancel,
                                     BroBroError **error);

G_END_DECLS
