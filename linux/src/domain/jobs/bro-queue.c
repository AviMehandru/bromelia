/* bro-queue.c */
#include "bro-queue.h"

#include <string.h>

static const char *const names[] = {
  "acquisition",
  "processing",
  "maintenance",
};

const char *
bro_queue_to_wire (BroQueue value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_queue_from_wire (const char *text, BroQueue *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroQueue) i;
      return TRUE;
    }
  return FALSE;
}
