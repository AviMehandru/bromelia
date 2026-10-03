/* bro-continuation-query.h: a TV disc whose episode numbering may continue another's: the show's name (after the
 * lookup), the title of its label, its place in the set (-1: none), and the highest episode already in its
 * media-server season folder (-1: none; with the folder, for the log). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name;
  char *label_title;
  int season, part, volume;
  int disc;
  char *season_folder; /* nullable */
  int season_folder_highest;
} BroContinuationQuery;

BroContinuationQuery *bro_continuation_query_new (const char *name, const char *label_title, int disc);
void bro_continuation_query_free (BroContinuationQuery *query);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroContinuationQuery, bro_continuation_query_free)

G_END_DECLS
