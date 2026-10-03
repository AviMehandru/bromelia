/* bro-track.c */
#include "bro-track.h"

void
bro_track_free (BroTrack *track)
{
  if (!track)
    return;
  g_free (track->codec);
  g_free (track->language);
  g_free (track->language_name);
  g_free (track->name);
  g_hash_table_unref (track->attributes);
  g_free (track);
}
