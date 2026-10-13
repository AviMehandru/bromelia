/* bro-sha256-hasher.h: BroSha256Hasher: SHA-256 fed a piece at a time (a file read back from disk as a stream),
 * lower-case hex at the end. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BroSha256Hasher BroSha256Hasher;

BroSha256Hasher *bro_sha256_hasher_new (void);
void bro_sha256_hasher_free (BroSha256Hasher *hasher);

void bro_sha256_hasher_update (BroSha256Hasher *self, const guint8 *bytes, gsize length);

/* The digest, lower-case hex (g_free it); the hasher is spent. */
char *bro_sha256_hasher_finish (BroSha256Hasher *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSha256Hasher, bro_sha256_hasher_free)

G_END_DECLS
