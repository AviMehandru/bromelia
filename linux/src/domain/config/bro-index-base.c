/* bro-index-base.c */
#include "bro-index-base.h"

#include <string.h>

static const char *const names[] = {
  "makemkv",
  "source",
};

const char *
bro_index_base_to_wire (BroIndexBase value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_index_base_from_wire (const char *text, BroIndexBase *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroIndexBase) i;
      return TRUE;
    }
  return FALSE;
}
