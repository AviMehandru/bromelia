/* bro-planned-output.h: one file a job will write, for bro_layouts_paths: its role, title or episode (-1: none),
 * the tokens of that file (track, episode, episodeNumber, episodeTitle, original, n, …), whether it's a movie's
 * main feature, and its extension (".mkv"; empty for a folder). */
#pragma once

#include "bro-path-role.h"

G_BEGIN_DECLS

typedef struct {
  BroPathRole role;
  GHashTable *values; /* char * → char * */
  int title;
  int episode;
  gboolean main_feature;
  char *extension;
} BroPlannedOutput;

BroPlannedOutput *bro_planned_output_new (BroPathRole role);
void bro_planned_output_free (BroPlannedOutput *output);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroPlannedOutput, bro_planned_output_free)

G_END_DECLS
