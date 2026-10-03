/* bro-simple-chapters.h: BroSimpleChapters, mkvextract chapters --simple output. */
#pragma once

#include "bro-duration.h"

G_BEGIN_DECLS

/* The chapter starts (BroDuration), in order: CHAPTER02=00:23:36.815 → 1416.815 s. */
GArray *bro_simple_chapters_parse (const char *text);

G_END_DECLS
