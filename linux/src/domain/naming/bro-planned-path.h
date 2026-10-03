/* bro-planned-path.h: a path relative to the library (folders separated by '/'), with its role and the title or
 * episode it holds (-1: none; the API's PlannedPath). */
#pragma once

#include "bro-path-role.h"

G_BEGIN_DECLS

typedef struct {
  char *path;
  BroPathRole role;
  int title;
  int episode;
} BroPlannedPath;

void bro_planned_path_free (BroPlannedPath *path);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroPlannedPath, bro_planned_path_free)

G_END_DECLS
