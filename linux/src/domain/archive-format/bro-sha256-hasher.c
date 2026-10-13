/* bro-sha256-hasher.c */
#include "bro-sha256-hasher.h"

struct _BroSha256Hasher {
  GChecksum *checksum;
};

BroSha256Hasher *
bro_sha256_hasher_new (void)
{
  BroSha256Hasher *h = g_new0 (BroSha256Hasher, 1);
  h->checksum = g_checksum_new (G_CHECKSUM_SHA256);
  return h;
}

void
bro_sha256_hasher_free (BroSha256Hasher *h)
{
  if (!h)
    return;
  g_checksum_free (h->checksum);
  g_free (h);
}

void
bro_sha256_hasher_update (BroSha256Hasher *self, const guint8 *bytes, gsize length)
{
  if (length > 0)
    g_checksum_update (self->checksum, bytes, (gssize) length);
}

char *
bro_sha256_hasher_finish (BroSha256Hasher *self)
{
  return g_strdup (g_checksum_get_string (self->checksum));
}
