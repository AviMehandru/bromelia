/* bro-mkv-track.h: a track of an MKV as mkvmerge -J lists it: its id and type (video, audio, subtitles). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int id;
  char *type;
} BroMkvTrack;

BroMkvTrack *bro_mkv_track_new (int id, const char *type);
void bro_mkv_track_free (BroMkvTrack *track);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMkvTrack, bro_mkv_track_free)

G_END_DECLS
