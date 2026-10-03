/* bro-mkv-probe.h: what mkvmerge -J says about a file: its duration (has_duration FALSE when it reports none), its
 * tracks and the number of chapters. */
#pragma once

#include "bro-mkv-track.h"

G_BEGIN_DECLS

typedef struct {
  gboolean has_duration;
  double duration_seconds;
  GPtrArray *tracks; /* BroMkvTrack * */
  int chapter_count;
} BroMkvProbe;

/* No duration, tracks or chapters. */
BroMkvProbe *bro_mkv_probe_new (void);
void bro_mkv_probe_free (BroMkvProbe *probe);

/* mkvmerge -J's JSON; NULL when it isn't JSON or mkvmerge didn't recognise the file. */
BroMkvProbe *bro_mkv_probe_parse (const char *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMkvProbe, bro_mkv_probe_free)

G_END_DECLS
