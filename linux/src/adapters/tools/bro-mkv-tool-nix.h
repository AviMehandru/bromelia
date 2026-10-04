/* bro-mkv-tool-nix.h: BroMkvToolNix: MKVToolNix (plan §10.1; shared/fixtures/adapters/mkvtoolnix.cases.json): mkvmerge -J
 * for the rip check, remuxing, splitting at chapters, chapter times with mkvextract. Exit status 1 is mkvmerge's
 * warning: the output is good. tool.missing when the locator can't find a tool. Synchronous. */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-file-system.h"
#include "bro-mkv-probe.h"
#include "bro-process-launcher.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MKV_TOOL_NIX (bro_mkv_tool_nix_get_type ())
G_DECLARE_FINAL_TYPE (BroMkvToolNix, bro_mkv_tool_nix, BRO, MKV_TOOL_NIX, GObject)

/* @work_directory: where chapter_times writes mkvextract's chapter file (the job's folder). Keeps references. */
BroMkvToolNix *bro_mkv_tool_nix_new (BroProcessLauncher *launcher, BroFileSystem *fs, BroToolLocator *locator, const char *work_directory);

/* NULL with @error unset when mkvmerge can't read the file; NULL with @error set when it can't run. */
BroMkvProbe *bro_mkv_tool_nix_probe (BroMkvToolNix *self, const char *file, BroCancellationToken *cancel, BroBroError **error);

/* Whether mkvmerge succeeded (FALSE with @error unset: it failed; with @error set: it couldn't run). */
gboolean bro_mkv_tool_nix_remux (BroMkvToolNix *self, const char *const *arguments, BroCancellationToken *cancel, BroBroError **error);

/* The parts in order (hidden files next to the input; GStrv); NULL when mkvmerge failed or made another number of
 * parts, which are removed. @chapters: int. */
GStrv bro_mkv_tool_nix_split (BroMkvToolNix *self, GArray *chapters, const char *input, BroCancellationToken *cancel, BroBroError **error);

/* BroDuration, in order; NULL when there are none to read. */
GArray *bro_mkv_tool_nix_chapter_times (BroMkvToolNix *self, const char *file, BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
