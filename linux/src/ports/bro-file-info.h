/* bro-file-info.h: A file's size, whether it is a folder, and when it last changed. */
#pragma once

#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gint64 size;
  gboolean is_directory;
  BroInstant modified_at;
} BroFileInfo;

/* Everything zero. */
BroFileInfo *bro_file_info_new (void);
void bro_file_info_free (BroFileInfo *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroFileInfo, bro_file_info_free)

G_END_DECLS
