/* bro-run-product.c */
#include "bro-run-product.h"

#include <string.h>

static const char *const names[] = {
  "nothing",
  "titles",
  "backup",
  "image",
};

const char *
bro_run_product_to_wire (BroRunProduct value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_run_product_from_wire (const char *text, BroRunProduct *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroRunProduct) i;
      return TRUE;
    }
  return FALSE;
}
