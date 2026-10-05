/* bro-data-image-copy.c */
#include "bro-data-image-copy.h"

void
bro_data_image_copy_free (BroDataImageCopy *copy)
{
  if (!copy)
    return;
  bro_bro_message_free (copy->warning);
  g_free (copy);
}
