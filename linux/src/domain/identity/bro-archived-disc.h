/* bro-archived-disc.h: an archived disc of a TV show (from its archive record): its place in the set (-1: none),
 * its highest episode (-1: none) and its folder. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name;
  char *label_title;
  int season, part, volume, disc;
  int last_episode;
  char *folder;
} BroArchivedDisc;

BroArchivedDisc *bro_archived_disc_new (const char *name, const char *label_title, const char *folder);
void bro_archived_disc_free (BroArchivedDisc *disc);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroArchivedDisc, bro_archived_disc_free)

G_END_DECLS
