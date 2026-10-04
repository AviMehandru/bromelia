/* bro-video-ts-byte-source.h: BroVideoTsByteSource: the Domain ByteSource over a DVD
 * (shared/fixtures/adapters/video-ts-byte-source.cases.json): VIDEO_TS inside an ISO 9660 / UDF bridge image (or a disc
 * device, read the same way), a VIDEO_TS folder, or the folder that contains one. Reads stop at the end of a file. A
 * BroByteSource comes first: pass the pointer itself wherever a BroByteSource * is wanted. */
#pragma once

#include "bro-byte-source.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct _BroVideoTsByteSource BroVideoTsByteSource;

/* NULL when @path is neither an image with VIDEO_TS nor a folder with (or that is) VIDEO_TS holding VIDEO_TS.IFO. */
BroVideoTsByteSource *bro_video_ts_byte_source_open (const char *path);

/* The image's volume name, or the name of the folder that contains VIDEO_TS. */
const char *bro_video_ts_byte_source_label (BroVideoTsByteSource *self);

void bro_video_ts_byte_source_free (BroVideoTsByteSource *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroVideoTsByteSource, bro_video_ts_byte_source_free)

G_END_DECLS
