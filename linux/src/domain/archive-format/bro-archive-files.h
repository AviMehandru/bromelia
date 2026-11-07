/* bro-archive-files.h: BroArchiveFiles, which files of a unit's folder are Bromelia's own, which are media-server
 * metadata, and which files the produced items are. */
#pragma once

#include "bro-id.h"

#include <glib.h>

G_BEGIN_DECLS

/* Files Bromelia writes next to the archived ones that SHA256SUMS doesn't list: SHA256SUMS, the records
 * (bromelia*.json), the logs (bromelia-log, makemkv-log, makemkv-debug-log; with a unit's short id in v3:
 * bromelia-1a2b3c4d-log.txt …), and the INCOMPLETE / READ ERRORS notes. */
gboolean bro_archive_files_is_own_file (const char *name);

/* The unit a job archives: a UUID (version 8, RFC 9562) made from the first 16 bytes of SHA-256 of
 * "bromelia-unit\n<jobId>", so a rerun of Seal names the same files (bromelia-<unit8>.json) without any randomness. */
BroId bro_archive_files_unit_id (const BroId *job_id);

/* What a write that was stopped leaves behind: .<name>.bromelia-tmp-<id> (FileSystem.writeAtomically) and
 * .<name>.part-<id> (DataImager). Startup recovery removes them. */
gboolean bro_archive_files_is_leftover (const char *name);

/* Files written for media servers, which SHA256SUMS doesn't list (servers may rewrite them): .nfo files and
 * poster.jpg. */
gboolean bro_archive_files_is_metadata_file (const char *name);

/* The files the produced items are: each produced file, and every file under a produced folder (from @tree, the
 * folder's files), relative paths in code point order, each once; hidden files (a component starting with a dot)
 * are left out. Free with g_strfreev. */
GStrv bro_archive_files_expand (const char *const *produced, const char *const *tree);

G_END_DECLS
