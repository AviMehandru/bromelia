/* bro-resumability.c */
#include "bro-resumability.h"

#include <string.h>

static const char *const names[] = {
  "idempotent",
  "resumeFrom",
  "notResumable",
};

const char *
bro_resumability_to_wire (BroResumability value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_resumability_from_wire (const char *text, BroResumability *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroResumability) i;
      return TRUE;
    }
  return FALSE;
}
