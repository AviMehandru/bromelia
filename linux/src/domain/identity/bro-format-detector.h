/* bro-format-detector.h: BroFormatDetector, which format a disc, ISO or backup is. */
#pragma once

#include "bro-disc-flags.h"
#include "bro-disc-format.h"
#include "bro-format-code.h"
#include "bro-listing.h"

G_BEGIN_DECLS

/* From the listing (nullable; MakeMKV's type, and a Blu-ray whose video is 2160p or HEVC is UHD), else from a
 * backup's structure (VIDEO_TS is a DVD; BDMV/index.bdmv starts with INDX0300 on UHD discs, INDX0200 on others),
 * else from the drive's flags (nullable); unknown otherwise. */
BroDiscFormat bro_format_detector_detect (const BroListing *listing, const BroDiscFlags *flags, const char *index_bdmv,
                                          gboolean has_video_ts);

/* DVD, BR, 4K, HDDVD or DISC; with an 'e' suffix when the backup isn't decrypted. */
BroFormatCode bro_format_detector_code (BroDiscFormat format, gboolean encrypted);

G_END_DECLS
