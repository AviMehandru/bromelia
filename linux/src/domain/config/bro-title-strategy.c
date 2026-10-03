/* bro-title-strategy.c */
#include "bro-title-strategy.h"

#include <string.h>

static const char *const names[] = {
  "all",
  "longest",
  "indices",
  "manual",
};

const char *
bro_title_strategy_to_wire (BroTitleStrategy value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_title_strategy_from_wire (const char *text, BroTitleStrategy *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroTitleStrategy) i;
      return TRUE;
    }
  return FALSE;
}
