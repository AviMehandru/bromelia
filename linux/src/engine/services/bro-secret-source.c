/* bro-secret-source.c */
#include "bro-secret-source.h"

#include <string.h>

static const char *const names[] = {
  "keystore",
  "environment",
  "file",
};

const char *
bro_secret_source_to_wire (BroSecretSource value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_secret_source_from_wire (const char *text, BroSecretSource *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0)
      {
        *out = (BroSecretSource) i;
        return TRUE;
      }
  return FALSE;
}
