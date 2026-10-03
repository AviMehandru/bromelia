/* bro-remux.h: BroRemux, mkvmerge arguments that keep only the chosen tracks of a file ripped with every track
 * selected. */
#pragma once

#include "bro-title.h"

G_BEGIN_DECLS

/* -o output --video-tracks … | --no-video, the same for audio and subtitles, input (a --no-… only for a type the
 * file has); NULL when the file's tracks (@layout: BroMkvTrack *) don't match the title's (another count, or a
 * video, audio or subtitle track of the listing where the file has another type). @keep (int) holds the title's
 * track indexes. */
GStrv bro_remux_arguments (GPtrArray *layout, const BroTitle *title, GArray *keep, const char *input, const char *output);

G_END_DECLS
