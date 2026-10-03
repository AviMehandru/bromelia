/* bro-split.h: BroSplit, mkvmerge arguments that split a file at chapters without re-encoding. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* -o output --split chapters:8,15,… input; mkvmerge numbers the parts in @output (-001, -002, …). @chapters: int. */
GStrv bro_split_arguments (GArray *chapters, const char *input, const char *output);

G_END_DECLS
