/* bro-title-choice.h: a title of an opened disc as the user left it: ticked or not, the streams kept when they were
 * picked by hand (NULL: MakeMKV's selection), and the output file name ("" = from the layout). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int index;
  gboolean selected;
  GArray *tracks; /* int; nullable */
  char *name;
} BroTitleChoice;

BroTitleChoice *bro_title_choice_new (int index, gboolean selected, GArray *tracks, const char *name); /* takes @tracks */
BroTitleChoice *bro_title_choice_copy (const BroTitleChoice *choice);
void bro_title_choice_free (BroTitleChoice *choice);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroTitleChoice, bro_title_choice_free)

G_END_DECLS
