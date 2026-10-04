/* bro-unit-file.h: A row of unit_files (the unit is implied). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *path; /* relative to the unit's folder */
  gint64 size;
  char *sha256;
  char *role;
  gboolean has_title;
  int title;
  gboolean has_episode;
  int episode;
} BroUnitFile;

/* Everything zero. */
BroUnitFile *bro_unit_file_new (void);
void bro_unit_file_free (BroUnitFile *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroUnitFile, bro_unit_file_free)

G_END_DECLS
