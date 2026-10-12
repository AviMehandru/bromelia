/* bro-probe-output.h: what ProbeStep found: the listing (NULL when a backup goes on without one), its fingerprint, the
 * LibreDrive detail, and whether the listing is the one the disc was opened with (a backup whose listing failed). */
#pragma once

#include "bro-json-value.h"
#include "bro-listing.h"

G_BEGIN_DECLS

typedef struct {
  BroListing *listing; /* nullable */
  char *fingerprint;   /* nullable */
  char *libre_drive;   /* nullable */
  gboolean from_session;
} BroProbeOutput;

/* Takes a reference to @listing. */
BroProbeOutput *bro_probe_output_new (BroListing *listing, const char *fingerprint, const char *libre_drive, gboolean from_session);
void bro_probe_output_free (BroProbeOutput *output);

BroJsonValue *bro_probe_output_encode (const BroProbeOutput *output);
BroProbeOutput *bro_probe_output_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroProbeOutput, bro_probe_output_free)

G_END_DECLS
