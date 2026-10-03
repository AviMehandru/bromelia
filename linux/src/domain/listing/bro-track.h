/* bro-track.h: a track (stream) of a title: its kind (attribute 1), codec (6, else 5), language (3, 4), name
 * (2), whether it's the default ('d' in 38), and every attribute MakeMKV reported. */
#pragma once

#include "bro-track-kind.h"

G_BEGIN_DECLS

typedef struct {
  int index;
  BroTrackKind kind;
  char *codec;
  char *language;
  char *language_name;
  char *name;
  gboolean is_default;
  GHashTable *attributes; /* GINT_TO_POINTER (BroAttributeId) → char * */
} BroTrack;

void bro_track_free (BroTrack *track);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroTrack, bro_track_free)

G_END_DECLS
