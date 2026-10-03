/* bro-drive-match.h: how a drive entry finds its drive: drive_name (MakeMKV's identification, compared case-
 * and space-insensitively), else device_path. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *drive_name;  /* never NULL */
  char *device_path; /* never NULL */
} BroDriveMatch;

BroDriveMatch *bro_drive_match_new (const char *drive_name, const char *device_path);
void bro_drive_match_free (BroDriveMatch *match);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveMatch, bro_drive_match_free)

G_END_DECLS
