/* bro-source-resolver.h: BroSourceResolver, where a file or folder the user opened leads: MakeMKV opens discs (an
 * image, or a folder holding BDMV, VIDEO_TS or HVDVD_TS), not single files, so a file inside a disc structure opens
 * the disc it belongs to. */
#pragma once

#include "bro-makemkv-source.h"

G_BEGIN_DECLS

/* An image (.iso, .img, .udf), else the folder holding BDMV / VIDEO_TS / HVDVD_TS, looking from the item itself up to
 * three levels, else the item as a folder. */
BroMakemkvSource *bro_source_resolver_source (const char *path, gboolean is_directory);

G_END_DECLS
