/* bro-archive-record-codec.h: BroArchiveRecordCodec, reads archive records (versions 2 and 3) and writes
 * version 3. */
#pragma once

#include "bro-archive-record.h"
#include "bro-archived-disc.h"

G_BEGIN_DECLS

/* A record of version 2 or 3; NULL when the bytes aren't one (not JSON, another format, another version). */
BroArchiveRecord *bro_archive_record_codec_decode (const char *bytes, gssize length);

/* A version 3 record: canonical JSON with the keys in archive-record-3.json's order. */
GBytes *bro_archive_record_codec_encode_v3 (const BroArchiveRecord *record);

/* The record as an archived TV disc for EpisodeContinuation: status success or errors, kind tv; the label's title
 * from the volume name (else the label); the highest episode number. NULL otherwise. */
BroArchivedDisc *bro_archive_record_codec_archived_disc (const BroArchiveRecord *record, const char *folder);

G_END_DECLS
