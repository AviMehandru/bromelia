/* bro-volume-info.h: the volume holding a path: a stable id, the bytes free, the file system and whether names are
 * case-sensitive. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *id;
  gint64 free_bytes;
  char *fs_type;
  gboolean case_sensitive;
} BroVolumeInfo;

/* Everything zero. */
BroVolumeInfo *bro_volume_info_new (void);
void bro_volume_info_free (BroVolumeInfo *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroVolumeInfo, bro_volume_info_free)

G_END_DECLS
