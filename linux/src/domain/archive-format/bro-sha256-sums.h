/* bro-sha256-sums.h: BroSha256Sums, SHA256SUMS, in the format sha256sum -c and shasum -a 256 -c read. */
#pragma once

#include "bro-sum-entry.h"

G_BEGIN_DECLS

#define BRO_SHA256_SUMS_FILE_NAME "SHA256SUMS"

/* "hash  path" or "hash *path" lines (BroSumEntry *); others are skipped. Hashes come back in lower case. */
GPtrArray *bro_sha256_sums_parse (const char *text);

/* "<sha256>  <path>" per line (@entries: BroSumEntry *), sorted by path in code point order, with a final newline. */
char *bro_sha256_sums_render (GPtrArray *entries);

/* The entries of @existing (an earlier job's in the same folder) and @entries; a path in both takes the new hash. The file
 * is rewritten from this, so a line that can't be read follows the entries as it is, byte for byte: it isn't Bromelia's to
 * drop or to repair. */
GBytes *bro_sha256_sums_merge (GBytes *existing, GPtrArray *entries);

/* SHA-256 of bytes in memory, lower-case hex (a file is hashed as a stream with BroSha256Hasher). */
char *bro_sha256_sums_hash (const guint8 *bytes, gsize length);

G_END_DECLS
