/* bro-makemkv-args.h: BroMakemkvArgs, makemkvcon command lines. */
#pragma once

#include "bro-bro-error.h"
#include "bro-makemkv-options.h"
#include "bro-makemkv-source.h"

G_BEGIN_DECLS

/* … info source. */
GStrv bro_makemkv_args_info (const BroMakemkvSource *source, const BroMakemkvOptions *options);

/* … mkv source title destination (title: an index or "all"). */
GStrv bro_makemkv_args_mkv (const BroMakemkvSource *source, const char *title, const char *destination, const BroMakemkvOptions *options);

/* … backup [--decrypt] disc:N destination. NULL and @error set (backup.needsDrive) for an image or folder: backup
 * takes disc:N only. */
GStrv bro_makemkv_args_backup (const BroMakemkvSource *source, gboolean decrypt, const char *destination, const BroMakemkvOptions *options,
                               BroBroError **error);

/* The drive scan: -r --cache=1 info disc:9999. */
GStrv bro_makemkv_args_scan_drives (void);

G_END_DECLS
