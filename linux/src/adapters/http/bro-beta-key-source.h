/* bro-beta-key-source.h: BroBetaKeySource: MakeMKV's free beta key (plan §10.2;
 * shared/fixtures/adapters/beta-key-source.cases.json): the forum post (BRO_BETA_KEY_PAGE_URL), read with
 * BetaKeyPage.parse. makemkv.betaKey.http for another status, .notFound when the page has no key, .fetchFailed (with
 * the reason) when there is no answer. Synchronous. */
#pragma once

#include "bro-http-client.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_BETA_KEY_SOURCE (bro_beta_key_source_get_type ())
G_DECLARE_FINAL_TYPE (BroBetaKeySource, bro_beta_key_source, BRO, BETA_KEY_SOURCE, GObject)

/* Keeps a reference to @http. */
BroBetaKeySource *bro_beta_key_source_new (BroHttpClient *http);

/* The key; NULL and @error set on failure. Free with g_free. */
char *bro_beta_key_source_current_key (BroBetaKeySource *self, BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
