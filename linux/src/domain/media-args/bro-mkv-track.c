/* bro-mkv-track.c */
#include "bro-mkv-track.h"

BroMkvTrack *
bro_mkv_track_new (int id, const char *type)
{
  BroMkvTrack *t = g_new0 (BroMkvTrack, 1);
  t->id = id;
  t->type = g_strdup (type ? type : "");
  return t;
}

void
bro_mkv_track_free (BroMkvTrack *track)
{
  if (!track)
    return;
  g_free (track->type);
  g_free (track);
}
