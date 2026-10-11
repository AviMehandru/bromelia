/* bro-matroska.h: BroMatroska: MKVToolNix (plan §10.1): what an MKV file holds, remuxing, splitting at chapters,
 * chapter times. Synchronous. BroMkvToolNix (Adapters) implements it. */
#pragma once

#include "bro-bro-error.h"
#include "bro-cancellation-token.h"
#include "bro-mkv-probe.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MATROSKA (bro_matroska_get_type ())
G_DECLARE_INTERFACE (BroMatroska, bro_matroska, BRO, MATROSKA, GObject)

struct _BroMatroskaInterface {
  GTypeInterface parent_iface;
  BroMkvProbe *(*probe) (BroMatroska *self, const char *file, BroCancellationToken *cancel, BroBroError **error);
  gboolean (*remux) (BroMatroska *self, const char *const *arguments, BroCancellationToken *cancel, BroBroError **error);
  GStrv (*split) (BroMatroska *self, GArray *chapters, const char *input, BroCancellationToken *cancel, BroBroError **error);
  GArray *(*chapter_times) (BroMatroska *self, const char *file, BroCancellationToken *cancel, BroBroError **error);
};

/* NULL with @error unset when mkvmerge can't read the file; NULL with @error set when it can't run. */
BroMkvProbe *bro_matroska_probe (BroMatroska *self, const char *file, BroCancellationToken *cancel, BroBroError **error);

/* Whether mkvmerge succeeded (FALSE with @error unset: it failed; with @error set: it couldn't run). */
gboolean bro_matroska_remux (BroMatroska *self, const char *const *arguments, BroCancellationToken *cancel, BroBroError **error);

/* The parts in order (GStrv); NULL when it failed or made another number of parts. @chapters: int. */
GStrv bro_matroska_split (BroMatroska *self, GArray *chapters, const char *input, BroCancellationToken *cancel, BroBroError **error);

/* BroDuration, in order; NULL when there are none to read. */
GArray *bro_matroska_chapter_times (BroMatroska *self, const char *file, BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
