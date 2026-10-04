/* bro-output-source.c */
#include "bro-output-source.h"

#include <string.h>

static const char *const names[] = {
  "stdout",
  "stderr",
};

const char *
bro_output_source_to_wire (BroOutputSource value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_output_source_from_wire (const char *text, BroOutputSource *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroOutputSource) i;
      return TRUE;
    }
  return FALSE;
}
