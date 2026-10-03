/* bro-fingerprint.h: BroFingerprint, disc fingerprints, to recognise a disc when it is inserted again. */
#pragma once

#include "bro-listing.h"

G_BEGIN_DECLS

/* v1, byte for byte as today: "v1:" and 32 hex digits of SHA-256 over the volume name (else the name), the
 * title count and, sorted, each title's source title id, length in seconds, segment map and size in bytes. NULL
 * for a listing without titles. Free with g_free. */
char *bro_fingerprint_of (const BroListing *listing);

G_END_DECLS
