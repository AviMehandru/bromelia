/* bro-planned-path.c */
#include "bro-planned-path.h"

void
bro_planned_path_free (BroPlannedPath *path)
{
  if (!path)
    return;
  g_free (path->path);
  g_free (path);
}
