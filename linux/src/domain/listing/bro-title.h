/* bro-title.h: a title of a listing: MakeMKV's index (it changes with the minimum length setting), the source
 * title (24) and file (16), name (2), comment (49), duration (9), chapters (8), size (11), segment map (26),
 * output file name (27), angle (15), its tracks, and every attribute MakeMKV reported. */
#pragma once

#include "bro-track.h"

G_BEGIN_DECLS

typedef struct {
  int index;
  gboolean has_source_title_id;
  int source_title_id;
  char *source_file;
  char *name;
  char *comment;
  char *duration;
  int duration_seconds;
  int chapters;
  gint64 size_bytes;
  char *segment_map;
  char *output_file_name;
  gboolean has_angle;
  int angle;
  GPtrArray *tracks;      /* BroTrack *, in index order */
  GHashTable *attributes; /* GINT_TO_POINTER (BroAttributeId) → char * */
} BroTitle;

void bro_title_free (BroTitle *title);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroTitle, bro_title_free)

G_END_DECLS
