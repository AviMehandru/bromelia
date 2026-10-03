/* bro-id.c */
#include "bro-id.h"

char *
bro_id_short (const BroId *id)
{
  GString *out = g_string_new (NULL);
  for (const char *c = id->value; *c && out->len < 8; c++)
    if (*c != '-')
      g_string_append_c (out, g_ascii_tolower (*c));
  return g_string_free (out, FALSE);
}
