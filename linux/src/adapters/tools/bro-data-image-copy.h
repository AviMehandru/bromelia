/* bro-data-image-copy.h: what a DataImager copy made: the bytes copied, and a warning when the image is complete but its
 * folder couldn't be flushed (fs.notSynced). */
#pragma once

#include "bro-bro-message.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gint64 bytes;
  BroBroMessage *warning; /* nullable */
} BroDataImageCopy;

void bro_data_image_copy_free (BroDataImageCopy *copy);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDataImageCopy, bro_data_image_copy_free)

G_END_DECLS
