/* bro-archived-disc.c */
#include "bro-archived-disc.h"

BroArchivedDisc *
bro_archived_disc_new (const char *name, const char *label_title, const char *folder)
{
  BroArchivedDisc *d = g_new0 (BroArchivedDisc, 1);
  d->name = g_strdup (name ? name : "");
  d->label_title = g_strdup (label_title ? label_title : "");
  d->season = d->part = d->volume = d->disc = d->last_episode = -1;
  d->folder = g_strdup (folder ? folder : "");
  return d;
}

void
bro_archived_disc_free (BroArchivedDisc *d)
{
  if (!d)
    return;
  g_free (d->name);
  g_free (d->label_title);
  g_free (d->folder);
  g_free (d);
}
