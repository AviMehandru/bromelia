/* bro-menu-ocr.h: BroMenuOcr: episode numbers from a DVD's menu stills (plan §10.1;
 * shared/fixtures/adapters/menu-ocr.cases.json): ffmpeg turns each still cell into a large grey PNG, tesseract reads
 * it, bro_menu_numbers_parse finds the numbers. Synchronous. */
#pragma once

#include "bro-byte-source.h"
#include "bro-cancellation-token.h"
#include "bro-file-system.h"
#include "bro-process-launcher.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MENU_OCR (bro_menu_ocr_get_type ())
G_DECLARE_FINAL_TYPE (BroMenuOcr, bro_menu_ocr, BRO, MENU_OCR, GObject)

/* Keeps references. */
BroMenuOcr *bro_menu_ocr_new (BroProcessLauncher *launcher, BroToolLocator *locator, BroFileSystem *fs);

/* The PNG of each cell (BroCellRef *) ffmpeg could convert, in order, in @dir (which must exist). NULL and @error set
 * (tool.missing, job.cancelled, or a write failure). */
GStrv bro_menu_ocr_extract_stills (BroMenuOcr *self, BroByteSource *source, GPtrArray *cells, const char *dir, BroCancellationToken *cancel,
                                   BroBroError **error);

/* The episode numbers (int) on @stills, each once, in the order first read. NULL and @error set (tool.missing,
 * job.cancelled). */
GArray *bro_menu_ocr_read_numbers (BroMenuOcr *self, const char *const *stills, BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
